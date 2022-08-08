/*
 Copyright 2022 Primihub

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

      https://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 */
#include <glog/logging.h>

#include "src/primihub/service/notify/model.h"

namespace primihub::service {

using primihub::rpc::NodeEventType;



//////////////////////////////EventBusNotifyDelegate/////////////////////////////////////////////////
EventBusNotifyDelegate::EventBusNotifyDelegate(const rpc::TaskContext &task_context) {
    
}

void EventBusNotifyDelegate::notifyStatus(const std::string task_id, const std::string &status) {
    // TODO send status to event bus.
    event_bus_.fire_event(TaskStatusEvent{task_id, status});
}

void EventBusNotifyDelegate::notifyResult(const std::string task_id, const std::string &result_dataset_url) {
    // TODO send result to event bus.
    event_bus_.fire_event(TaskResultEvent{task_id, result_dataset_url});
    
}

/////////////////////////////////////EventBusNotifyServerSubscriber//////////////////////////////////////////

EventBusNotifyServerSubscriber::EventBusNotifyServerSubscriber(const std::shared_ptr<NotifyServer> &notify_server,
                                                            primihub::common::event_bus *event_bus_ptr)
: NotifyServerSubscriber(notify_server), event_bus_ptr_(event_bus_ptr),
  task_stauts_reg_(std::move(event_bus_ptr_->register_handler<TaskStatusEvent>(
        notify_server_.get(), &NotifyServer::onTaskStatusEvent))),
   task_result_reg_(std::move(event_bus_ptr_->register_handler<TaskResultEvent>(
        notify_server_.get(), &NotifyServer::onTaskResultEvent))) {

} 

EventBusNotifyServerSubscriber::~EventBusNotifyServerSubscriber() {
    event_bus_ptr_->remove_handler(task_stauts_reg_);
    event_bus_ptr_->remove_handler(task_result_reg_);
}

//////////////////////////////////////////GRPCNotifyServer////////////////////////////////////////////////////

 // Event handlers
void GRPCNotifyServer::onTaskStatusEvent(const TaskStatusEvent &e) {
    // TODO put TaskStatus event to session message queue if session is exist.
    // find which session subscribed this task.
    auto session = getSessionByTaskId(e.task_id);
    if (session) {
        // Construct NodeEventReply rpc message.
        auto new_message = std::make_shared<primihub::rpc::NodeEventReply>();
        new_message->set_event_type(rpc::NodeEventType::NODE_EVENT_TYPE_TASK_STATUS);
        primihub::rpc::TaskStatus *task_status = new_message->mutable_task_status();
        primihub::rpc::TaskContext *task_context = task_status->mutable_task_context();
        task_context->set_task_id(e.task_id);
        task_context->set_node_id(this->node_id_);
        task_context->set_job_id(e.job_id);
        task_status->set_allocated_task_context(task_context);
        task_status->set_status(e.status);
        new_message->set_allocated_task_status(task_status);

        session->putMessage(new_message);
    }
}

void GRPCNotifyServer::onTaskResultEvent(const TaskResultEvent &e) {
    // TODO put TaskResult event to session message queue if session is exist.
     auto session = getSessionByTaskId(e.task_id);
    if (session) {
        // Construct NodeEventReply rpc message.
        auto new_message = std::make_shared<primihub::rpc::NodeEventReply>();
        new_message->set_event_type(rpc::NodeEventType::NODE_EVENT_TYPE_TASK_RESULT);
        primihub::rpc::TaskResult *task_result = new_message->mutable_task_result();
        primihub::rpc::TaskContext *task_context = task_result->mutable_task_context();
        task_context->set_task_id(e.task_id);
        task_context->set_node_id(this->node_id_);
        task_context->set_job_id(e.job_id);
        task_result->set_allocated_task_context(task_context);
        task_result->set_result_dataset_url(e.result_dataset_url);
        new_message->set_allocated_task_result(task_result);

        session->putMessage(new_message);
    }
}


bool GRPCNotifyServer::init(const std::string &server, 
    const std::shared_ptr<NotifyServerSubscriber> &notify_server_subscriber) {
    ::grpc::ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server, grpc::InsecureServerCredentials());
    // Register "service_" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *asynchronous* service.
    builder.RegisterService(&node_async_service_);
    // Get hold of the completion queue used for the asynchronous communication
    // with the gRPC runtime.
    completion_queue_call_ = builder.AddCompletionQueue();
    completion_queue_notification_ = builder.AddCompletionQueue();
    // Finally assemble the server.
    server_ = builder.BuildAndStart();
    LOG(INFO) << "NodeService listening on: " << server;
    return true;
}


void GRPCNotifyServer::run() {
    running_ = true;
    std::thread thread_completion_queue_call([this] {
        // spawn the first rpc call session
        addSession();
        uint64_t session_id;  // uniquely identifies a request.
        bool ok;
        // rpc event "read done / write done / close(already connected)" call-back by this completion queue
        while (completion_queue_call_->Next(reinterpret_cast<void **>(&session_id), &ok)) {
            auto event = static_cast<GrpcNotifyEvent>(session_id & GRPC_NOTIFY_EVENT_MASK);
            session_id = session_id >> GRPC_NOTIFY_EVENT_BIT_LENGTH;
            LOG(INFO) << "session_id_: " << session_id << ", completion queue(call), event: " << event;
            if (event == GRPC_NOTIFY_EVENT_FINISHED) {
                LOG(INFO) << "session_id_: " << session_id << ", event: " << event;
                removeSession(session_id);
                continue;
            }
            auto session = getSession(session_id);
            if (session == nullptr) {
                LOG(INFO) << "session_id_: " << session_id << ", have been removed";
                continue;
            }
            if (!ok) {
                LOG(INFO) << "session_id_: " << session_id << ", rpc call closed";
                removeSession(session_id);
                continue;
            }
            {
                std::lock_guard<std::mutex> local_lock_guard{session->mutex_};
                session->process(event);
            }
        }
        LOG(INFO) << "completion queue(call) exit";
    });
    std::thread thread_completion_queue_notification([this] {
        uint64_t session_id;  // uniquely identifies a request.
        bool ok;
        // rpc event "new connection / close(waiting for connect)" call-back by this completion queue
        while (completion_queue_notification_->Next(reinterpret_cast<void **>(&session_id), &ok)) {
            auto event = static_cast<GrpcNotifyEvent>(session_id & GRPC_NOTIFY_EVENT_MASK);
            session_id = session_id >> GRPC_NOTIFY_EVENT_BIT_LENGTH;
            LOG(INFO) << "session_id_: " << session_id
                      << ", completion queue(notification), event: " << event;
            if (event == GRPC_NOTIFY_EVENT_FINISHED) {
                removeSession(session_id);
                continue;
            }
            auto session = getSession(session_id);
            if (session == nullptr) {
                LOG(INFO) << "session_id_: " << session_id << ", have been removed";
                continue;
            }
            if (!ok) {
                LOG(INFO) << "session_id_: " << session_id << ", rpc call closed";
                removeSession(session_id);
                continue;
            }
            {
                std::lock_guard<std::mutex> local_lock_guard{session->mutex_};
                // deal request
                session->process(event);
            }
        }
        LOG(INFO) << "completion queue(notification) exit";
    });
    std::thread thread_publish_node_evt([this] {
        while (running_) {
            // TODO 此处应改为有session中消息队列中有TaskEvent时向client写入 
            // {
            //     std::lock_guard<std::mutex> local_lock_guard{mutex_sessions_};
            //     for (const auto &it : sessions_) {
            //         std::lock_guard<std::mutex> inner_local_lock_guard{it.second->mutex_};
            //         it.second->reply();
            //     }
            // }
        }
    });
    if (thread_completion_queue_call.joinable()) { thread_completion_queue_call.join(); }
    if (thread_completion_queue_notification.joinable()) { thread_completion_queue_notification.join(); }
    if (thread_publish_node_evt.joinable()) { thread_publish_node_evt.join(); }
    LOG(INFO) << "greeting server run() exit";
}


std::shared_ptr<GRPCClientSession> GRPCNotifyServer::addSession() {
    auto new_session_id = session_id_allocator_++; 
    auto new_session = std::make_shared<GRPCClientSession>(new_session_id);
    if (!new_session->init()) {
        LOG(ERROR) << "init new session failed";
        return nullptr;
    }
    std::shared_lock lock(mutex_sessions_);
    sessions_[new_session_id] = new_session;
    LOG(INFO) << "session_id_: " << new_session_id << ", spawn new session and wait for connect";
    return new_session;
}

void GRPCNotifyServer::removeSession(uint64_t sessionId) {
    std::shared_lock lock(mutex_sessions_);
    sessions_.erase(sessionId);
}

std::shared_ptr<GRPCClientSession> GRPCNotifyServer::getSession(uint64_t sessionId) {
    std::shared_lock lock(mutex_sessions_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) { return nullptr; }
    return it->second;
}

std::shared_ptr<GRPCClientSession> GRPCNotifyServer::getSessionByTaskId(const std::string taskId) {
    std::shared_lock lock(mutex_task_sessions_);
    auto it = task_sessions_.find(taskId);
    if (it == task_sessions_.end()) { return nullptr; }
    return it->second;
}


//////////////////////////////GRPCClientSession////////////////////////////////////////////////

bool GRPCClientSession::init() {
    GRPCNotifyServer::getInstance().node_async_service_.RequestSubscribeNodeEvent(
        &server_context_,
        &client_context_,
        &subscribe_stream,
        GRPCNotifyServer::getInstance().completion_queue_call_.get(),
        GRPCNotifyServer::getInstance().completion_queue_notification_.get(),
        reinterpret_cast<void*>(session_id_ << GRPC_NOTIFY_EVENT_BIT_LENGTH | GRPC_NOTIFY_EVENT_CONNECTED));
    return true;
} 


void GRPCClientSession::process(GrpcNotifyEvent event) {
    LOG(INFO) << "session_id_: " << session_id_ << ", current status: " << status_ << ", event: " << event;
    switch (event) {
        case GRPC_NOTIFY_EVENT_CONNECTED:
            // subscribe_stream.Read(
            //     &request_,
            //     reinterpret_cast<void*>(session_id_ << GRPC_NOTIFY_EVENT_BIT_LENGTH | GRPC_NOTIFY_EVENT_READ_DONE));
            status_ = GrpcClientSessionStatus::READY_TO_WRITE;
            GRPCNotifyServer::getInstance().addSession();
            return;
        case GRPC_NOTIFY_EVENT_READ_DONE:
            // LOG(DEBUG) << "session_id_: " << session_id_ << ", new request: " << request_.ShortDebugString();
            // performance_.setName(name_);
            // performance_.add(std::chrono::duration_cast<std::chrono::nanoseconds>(
            //                      std::chrono::system_clock::now().time_since_epoch())
            //                      .count() -
            //                  request_.current_nanosecond());
            // name_ = request_.name();
            // subscribe_stream.Read(
            //     &request_,
            //     reinterpret_cast<void*>(session_id_ << GRPC_EVENT_BIT_LENGTH | GRPC_EVENT_READ_DONE));
            // TODO 不需要通过read client请求返回，当消息队列中有item时立刻reply
            // reply();
            return;
        case GRPC_NOTIFY_EVENT_WRITE_DONE:
            if (!message_queue_.empty()) {
                status_ = GrpcClientSessionStatus::WAIT_WRITE_DONE;
                subscribe_stream.Write(
                    *message_queue_.front(),
                    reinterpret_cast<void*>(session_id_ << GRPC_NOTIFY_EVENT_BIT_LENGTH | GRPC_NOTIFY_EVENT_WRITE_DONE));
                message_queue_.pop_front();
            } else {
                status_ = GrpcClientSessionStatus::READY_TO_WRITE;
            }
            return;
        default:
            LOG(INFO) << "session_id_: " << session_id_ << ", unhandled event: " << event;
            return;
    }
}


// void GRPCClientSession::reply() {
//     if (status_ != GrpcClientSessionStatus::READY_TO_WRITE && status_ != GrpcClientSessionStatus::WAIT_WRITE_DONE) {
//         return;
//     }
//     // auto new_message = std::make_shared<primihub::rpc::NodeEventReply>();
//     //TODO  get reply from queue message front
    
//     if (status_ == GrpcSessionStatus::READY_TO_WRITE) {
//         status_ = GrpcSessionStatus::WAIT_WRITE_DONE;
//         subscribe_stream.Write(
//             *new_message,
//             reinterpret_cast<void*>(session_id_ << GRPC_EVENT_BIT_LENGTH | GRPC_EVENT_WRITE_DONE));
//     } else {
//         message_queue_.emplace_back(new_message);
//     }
// }

void GRPCClientSession::finish() {
    if (status_ == GrpcClientSessionStatus::WAIT_CONNECT) { return; }
    subscribe_stream.Finish(
        ::grpc::Status::CANCELLED,
        reinterpret_cast<void*>(session_id_ << GRPC_NOTIFY_EVENT_BIT_LENGTH | GRPC_NOTIFY_EVENT_FINISHED));
}

void GRPCClientSession::putMessage(const std::shared_ptr<primihub::rpc::NodeEventReply> &message) {
    if (status_ != GrpcClientSessionStatus::READY_TO_WRITE) {
        message_queue_.emplace_back(message);
        return;
    }
    status_ = GrpcClientSessionStatus::WAIT_WRITE_DONE;
    subscribe_stream.Write(
        *message,
        reinterpret_cast<void*>(session_id_ << GRPC_NOTIFY_EVENT_BIT_LENGTH | GRPC_NOTIFY_EVENT_WRITE_DONE));
}

/////////////////////////stream methods////////////////////////////////////////////////////////

std::ostream& operator<<(std::ostream& os, GrpcNotifyEvent event) {
    // omit default case to trigger compiler warning for missing cases
    switch (event) {
        case GrpcNotifyEvent::GRPC_NOTIFY_EVENT_CONNECTED:
            return os << "GRPC_EVENT_CONNECTED";
        case GrpcNotifyEvent::GRPC_NOTIFY_EVENT_READ_DONE:
            return os << "GRPC_EVENT_READ_DONE";
        case GrpcNotifyEvent::GRPC_NOTIFY_EVENT_WRITE_DONE:
            return os << "GRPC_EVENT_WRITE_DONE";
        case GrpcNotifyEvent::GRPC_NOTIFY_EVENT_FINISHED:
            return os << "GRPC_EVENT_FINISHED";
    }
}

std::ostream& operator<<(std::ostream& os, GrpcClientSessionStatus sessionStatus) {
    // omit default case to trigger compiler warning for missing cases
    switch (sessionStatus) {
        case GrpcClientSessionStatus::WAIT_CONNECT:
            return os << "WAIT_CONNECT";
        case GrpcClientSessionStatus::READY_TO_WRITE:
            return os << "READY_TO_WRITE";
        case GrpcClientSessionStatus::WAIT_WRITE_DONE:
            return os << "WAIT_WRITE_DONE";
        case GrpcClientSessionStatus::FINISHED:
            return os << "FINISHED";
    }
}


} // namespace primihub::service

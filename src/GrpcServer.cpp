#include "GrpcServer.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// CameraControlServiceImpl
// ─────────────────────────────────────────────────────────────────────────────

CameraControlServiceImpl::CameraControlServiceImpl(SpinnakerCameraManager& cam_mgr,
                                                   SharedMemoryManager&    shm_mgr)
    : cam_mgr_(cam_mgr), shm_mgr_(shm_mgr) {}

grpc::Status CameraControlServiceImpl::GetSystemState(
    grpc::ServerContext*,
    const camaramodule::Empty*,
    camaramodule::SystemState* resp)
{
    if (cam_mgr_.IsAcquiring()) {
        resp->set_status("ACQUIRING");
    } else if (cam_mgr_.GetConnectedCameraCount() > 0) {
        resp->set_status("IDLE");
    } else {
        resp->set_status("ERROR");
    }
    resp->set_connected_cameras(cam_mgr_.GetConnectedCameraCount());
    resp->set_current_fps(cam_mgr_.GetCurrentFPS());
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::StartAcquisition(
    grpc::ServerContext*,
    const camaramodule::Empty*,
    camaramodule::CommandStatus* resp)
{
    const bool ok = cam_mgr_.StartAcquisition();
    resp->set_success(ok);
    resp->set_message(ok ? "Acquisition started." : "Failed to start acquisition.");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::StopAcquisition(
    grpc::ServerContext*,
    const camaramodule::Empty*,
    camaramodule::CommandStatus* resp)
{
    const bool ok = cam_mgr_.StopAcquisition();
    resp->set_success(ok);
    resp->set_message(ok ? "Acquisition stopped." : "Failed to stop acquisition.");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetParameter(
    grpc::ServerContext*,
    const camaramodule::ParameterRequest* req,
    camaramodule::CommandStatus*          resp)
{
    const bool ok = cam_mgr_.SetParameter(
        req->param_name(), req->float_value(), req->int_value());
    resp->set_success(ok);
    resp->set_message(ok ? "Parameter set." : "Parameter not found or not writable.");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::TriggerDiskSave(
    grpc::ServerContext*,
    const camaramodule::Empty*,
    camaramodule::CommandStatus* resp)
{
    cam_mgr_.TriggerDiskSave();
    resp->set_success(true);
    resp->set_message("Disk save queued for next captured frame.");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetLatestImageFrame(
    grpc::ServerContext*,
    const camaramodule::FrameRequest* req,
    camaramodule::FrameInfo*          resp)
{
    // camera_id == 0 is the proto3 default (first camera).
    // Callers that want "any camera" must explicitly send -1.
    const int32_t camera_id = req->camera_id();
    const int32_t idx       = shm_mgr_.AcquireLatestFrame(camera_id);

    if (idx < 0) {
        const std::string msg = (camera_id < 0)
            ? "No frame available from any camera."
            : "No frame available from camera " + std::to_string(camera_id) + ".";
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, msg);
    }

    const SharedMemoryHeader* hdr = shm_mgr_.GetHeader();
    const int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();

    resp->set_shared_memory_index(idx);
    resp->set_timestamp(ts);
    resp->set_width(hdr->image_width);
    resp->set_height(hdr->image_height);
    resp->set_camera_id(hdr->buffer_camera_id[idx]);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::ReleaseImageFrame(
    grpc::ServerContext*,
    const camaramodule::ReleaseRequest* req,
    camaramodule::CommandStatus*        resp)
{
    shm_mgr_.ReleaseFrame(req->shared_memory_index());
    resp->set_success(true);
    resp->set_message("Frame released.");
    return grpc::Status::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// GrpcServer
// ─────────────────────────────────────────────────────────────────────────────

GrpcServer::GrpcServer(SpinnakerCameraManager& cam_mgr, SharedMemoryManager& shm_mgr)
    : cam_mgr_(cam_mgr), shm_mgr_(shm_mgr) {}

GrpcServer::~GrpcServer() {
    Shutdown();
}

void GrpcServer::Start(const std::string& listen_address) {
    service_ = std::make_unique<CameraControlServiceImpl>(cam_mgr_, shm_mgr_);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
    builder.RegisterService(service_.get());
    builder.AddChannelArgument(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, -1); // unlimited

    server_ = builder.BuildAndStart();
    if (!server_) {
        throw std::runtime_error("Failed to start gRPC server on " + listen_address);
    }

    std::cout << "[GrpcServer] Listening on " << listen_address << '\n';
    server_->Wait(); // blocks until Shutdown() is called
}

void GrpcServer::Shutdown() {
    if (server_) {
        server_->Shutdown();
        server_.reset();
    }
}

#ifndef RATS_SERVICE_DATABASE_WORKER_H
#define RATS_SERVICE_DATABASE_WORKER_H

#include <atomic>
#include <memory>

namespace rats::service {

// A cancellation flag shared between whoever asks for a stop and the worker that
// has to notice it.
//
// It is a separate object rather than a member of the service on purpose: the
// service can start a new operation while an old worker is still unwinding, and a
// flag owned by the service would then be reset under the old worker's feet — it
// would never see the cancel and would run to completion, finishing an operation
// that no longer exists. A token belongs to exactly one operation and dies with
// it, so a stale worker keeps looking at the flag that was raised for *it*.
class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) { }

    void cancel() const { flag_->store(true); }
    bool cancelled() const { return flag_->load(); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

} // namespace rats::service

#endif // RATS_SERVICE_DATABASE_WORKER_H

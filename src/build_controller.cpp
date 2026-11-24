#include "aiws/gui/build_controller.hpp"
#include "aiws/chunker.hpp"
#include "aiws/context_builder.hpp"
#include "aiws/retrieval_engine.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace aiws::gui{
namespace{
// Add delay and cancellation without changing ProcessingCore.
class BuildChunker final : public ChunkingStrategy{
public:
    BuildChunker(int delay_ms, std::shared_ptr<const std::atomic_bool> cancelled)
        : delay_ms_(std::max(0, delay_ms)), cancelled_(std::move(cancelled)),
          chunker_(ChunkingPolicy{ProcessingCore::kMaxChunkTokens, ProcessingCore::kChunkOverlap,
                                  ProcessingCore::kParagraphPreferenceWindow}){
    }

    std::vector<Chunk> chunk(const Document& document, std::size_t order) const override{
        check_cancelled();
        auto chunks = chunker_.chunk(document, order);
        for(int remaining = delay_ms_; remaining > 0;){
            check_cancelled();
            const int slice = std::min(remaining, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            remaining -= slice;
        }
        check_cancelled();
        return chunks;
    }

private:
    void check_cancelled() const{
        #ifndef AIWS_FAULT_NO_CANCEL
        if(cancelled_->load()){
            throw std::runtime_error("Build cancelled");
        }
#endif
    }
    int delay_ms_;
    std::shared_ptr<const std::atomic_bool> cancelled_;
    Chunker chunker_;
};
}

BuildController::BuildController(Started started, Completed completed) : started_(std::move(started)), completed_(std::move(completed)){
}

BuildController::~BuildController(){
    stop_and_join(false);
}

void BuildController::start(Workspace snapshot, std::size_t id, int delay_ms){
    Request request{std::move(snapshot), id, delay_ms, std::make_shared<std::atomic_bool>(false)};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_ || pending_){
            throw std::logic_error("Cannot submit build request");
        }
        if(!worker_.joinable()){
            worker_ = std::thread(&BuildController::run, this);
        }
        pending_.emplace(std::move(request));
    }
    ready_.notify_one();
}

void BuildController::cancel(){
    std::optional<Request> discarded;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        discarded.swap(pending_);
        if(active_token_){
            active_token_->store(true);
        }
    }

    if(discarded){
        completed_(Result{discarded->id, nullptr, {}, true});
    }
}

void BuildController::shutdown() noexcept{
#ifdef AIWS_FAULT_SILENT_SHUTDOWN
    stop_and_join(true);
#else
    stop_and_join(false);
#endif
}

void BuildController::stop_and_join(bool simulate_silent_shutdown) noexcept{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!simulate_silent_shutdown || pending_ || active_token_){
            stopping_ = true;
            pending_.reset();
            if(active_token_){
                active_token_->store(true);
            }
            ready_.notify_one();
        }
    }
    if(worker_.joinable()){
        worker_.join();
    }
}

void BuildController::run(){
    for(;;){
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this](){ return stopping_ || pending_.has_value(); });
        if(stopping_){
            return;
        }
        Request request = std::move(*pending_);
        pending_.reset();
        active_token_ = request.cancelled;
        lock.unlock();
        auto result = build(request);
        lock.lock();
        active_token_.reset();
        const bool stopping = stopping_;
        lock.unlock();
        if(!stopping){
            completed_(std::move(result));
        }
    }
}

BuildController::Result BuildController::build(const Request& request){
    Result result;
    result.id = request.id;
    if(request.cancelled->load()){
        result.cancelled = true;
        return result;
    }
    started_(request.id);
    try{
        auto core = std::make_unique<ProcessingCore>(
            std::make_unique<BuildChunker>(request.delay_ms, request.cancelled),
            std::make_unique<RetrievalEngine>(), std::make_unique<ContextBuilder>());
        core->rebuild(request.snapshot);
        result.core = std::move(core);
    }catch(const std::exception& error){
        result.error = error.what();
    }catch(...){
        result.error = "Unknown build error";
    }

    result.cancelled = request.cancelled->load();
    if(result.cancelled){
        result.core.reset();
        result.error.clear();
    }
    return result;
}

} // namespace aiws::gui

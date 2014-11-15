//Author: Ugo Varetto
//File streaming service: wait for and stream content of files to clients over websockets

// clang++ -std=c++11 -I ../websocketplus/src -I /usr/local/libwebsockets/include  \
// ../src/streamer.cpp ../websocketplus/src/WebSocketService.cpp \
// -L /usr/local/libwebsockets/lib -lwebsockets

#include <string>
#include <iostream>
#include <vector>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <thread>
#include <fstream>
#include <chrono>
#include <future>
#include <functional>

#include "../websocketplus/src/WebSocketService.h"
#include "../websocketplus/src/Context.h"
#include "../websocketplus/src/examples/SessionService.h"

using namespace std;

//------------------------------------------------------------------------------
template < typename T >
class SyncQueue {
public:
    void Push(const T& e) {
        //simple scoped lock: acquire mutex in constructor,
        //release in destructor
        std::lock_guard< std::mutex > guard(mutex_);
        queue_.push_front(e);
        cond_.notify_one(); //notify 
    }
    T Pop() {
        //cannot use simple scoped lock here because lock passed to
        //wait must be able to acquire and release the mutex 
        std::unique_lock< std::mutex > lock(mutex_);
        //stop and wait for notification if condition is false;
        //continue otherwise
        cond_.wait(lock, [this]{ return !queue_.empty();});
        T e = queue_.back();
        queue_.pop_back();
        return e;
    }
    bool Empty() const {
        std::lock_guard< std::mutex > guard(mutex_);
        const bool e = queue_.empty();
        return e;
    }
private:
    void Clear() { queue_.clear(); }    
private:
    std::deque< T > queue_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
};
//------------------------------------------------------------------------------
using File = vector< char >;
using FileQueue = SyncQueue< shared_ptr< const File > >;
using SessionToQueue = unordered_map< const void*, shared_ptr< FileQueue > >;
//------------------------------------------------------------------------------
class SessionQueues {
public:
    shared_ptr< FileQueue >
    Map(const void* user, int queueSize = -1 /*not used*/) {
        std::lock_guard< std::mutex > guard(mutex_);
        if(stoq_.find(user) != stoq_.end()) Remove(user);
        shared_ptr< FileQueue > q(new FileQueue());
        stoq_[user] = q;
        return q;
    }
    void Remove(const void* user) {
        std::lock_guard< std::mutex > guard(mutex_);
        if(stoq_.find(user) != stoq_.end()) return;
        stoq_.erase(stoq_.find(user));
    }
    void Put(shared_ptr< File > f) {
        std::lock_guard< std::mutex > guard(mutex_);
        for(auto i: stoq_) i.second->Push(f);
    }
    shared_ptr< FileQueue > Get(const void* user) {
        std::lock_guard< std::mutex > guard(mutex_);
        if(stoq_.find(user) == stoq_.end())
            throw std::range_error("Missing user session");
        shared_ptr< FileQueue > q = stoq_.find(user)->second;
        return q;
    }
private:
    SessionToQueue stoq_;
    mutex mutex_;
};
//------------------------------------------------------------------------------
class FileStreamingService 
    : public SessionService< wsp::Context< shared_ptr< SessionQueues  > > > {

    using Context = wsp::Context< shared_ptr< SessionQueues > >;

public:

    using DataFrame = SessionService::DataFrame;
    FileStreamingService(Context* c) :
     SessionService(c), ctx_(c) {
        //the internal shared ptr counter is synchronized, so
        //no problem if multiple threads execute the following line
        //concurrently
        shared_ptr< SessionQueues > q = ctx_->GetServiceData();
        queue_ = q->Map(this);
        InitDataFrame();
    }
    bool Data() const override {
        if(df_.frameEnd < df_.bufferEnd) return true;
        else {
            InitDataFrame();
            return false;
        }
    }
    //return data frame and update frame end
    const DataFrame& Get(int requestedChunkLength) {
        if(df_.frameEnd < df_.bufferEnd) {
           //frameBegin *MUST* be updated in the UpdateOutBuffer method
           //because in case the consumed data is less than requestedChunkLength
           df_.frameEnd += min((ptrdiff_t) requestedChunkLength,
                               df_.bufferEnd - df_.frameEnd);
        } else {
           InitDataFrame();
        }
        return df_;
    }
    //update frame begin/end
    void UpdateOutBuffer(int bytesConsumed) {
        df_.frameBegin += bytesConsumed;
        df_.frameEnd = df_.frameBegin;
    }
    //streaming: always in send mode, no receive
    bool Sending() const override { return true; }
    void Put(void* p, size_t len, bool done) override {
        throw logic_error("Put is not implemented");
    }
    std::chrono::duration< double >
    MinDelayBetweenWrites() const {
        return std::chrono::duration< double >(0.0);
    }
private:
    void InitDataFrame() const {
        buffer_.reset();
        df_ = DataFrame(); //reset 
        if(queue_->Empty()) return;
        buffer_ = queue_->Pop();
        df_.bufferBegin = &buffer_->front();
        df_.bufferEnd = df_.bufferBegin + buffer_->size();
        df_.frameBegin = df_.bufferBegin;
        df_.frameEnd = df_.frameBegin;
        df_.binary = true;
    }
private:
    mutable DataFrame df_;
    mutable Context* ctx_ = nullptr;
    mutable shared_ptr< FileQueue > queue_;
    mutable shared_ptr< const File > buffer_;
};
//------------------------------------------------------------------------------
///@todo consider adding a name filter (0001 -> 1) 
///and a content filter (double array, color map -> turbojpeg -> jpeg)
void ReadFileService(string path,
                     string prefix,
                     int startFrame,
                     int period,
                     const string& suffix,
                     const bool& startService, 
                     const bool& stopService,
                     SessionQueues& q) {
    if(path.size() < 1) throw logic_error("Invalid  path size");
    if(path[path.size()-1] != '/') path += '/';
    prefix = path + prefix;
    int retries = 5;//@todo: make it a parameter; use negative (-1) for infinite
    int throttleInterval = 1;
    int retryInterval = 2;
    //make sure all the writes have already happened
    atomic_thread_fence(memory_order_acquire);
    while(!startService); //wait for a start request
    while(!stopService && retries != 0) {
        const string fname = prefix + to_string(startFrame) + suffix;
        ifstream in(fname, std::ifstream::in
                    | std::ifstream::binary);
        if(!in) {
            --retries;
            this_thread::sleep_for(chrono::seconds(retryInterval));
            continue;
        }
        in.seekg(0, ios::end);
        const size_t fileSize = in.tellg();
        in.seekg(0, ios::beg);
        shared_ptr< File > buf(new File(fileSize));
        in.read(&buf->front(), buf->size());
        q.Put(buf);
        startFrame = (startFrame + 1) % period;
        this_thread::sleep_for(chrono::seconds(throttleInterval));
    }
    if(retries == 0) throw logic_error("No file");
}
//------------------------------------------------------------------------------
int main(int argc, char** argv) {
    if(argc != 7) {
        std::cout << "usage: " 
                  << argv[0]
                  << " <path> <prefix> <start frame #> <period>"
                     " <suffix> <port>\n";
        return 1;
    }
    const string path = argv[1];
    const string prefix = argv[2];
    const string suffix = argv[5];
    const int startFrame = stoi(argv[3]); //throws if arg not valid
    const int port = stoi(argv[6]);
    const int period = stoi(argv[4]);
    const int maxSize = 100;

    bool stopService = false;
    bool startService = true; //start immediately
    atomic_thread_fence(memory_order_release);

    using SessionQueuesPtr = shared_ptr< SessionQueues >;
    using FStreamContext = wsp::Context< SessionQueuesPtr >;
    using WSS = wsp::WebSocketService;

    SessionQueuesPtr queue(new SessionQueues);
   
    auto fileService = async(launch::async,
                             ReadFileService,
                             path,
                             prefix,
                             startFrame,
                             period,
                             suffix,
                             cref(startService),
                             cref(stopService),
                             ref(*queue));

    WSS ws;
    ws.Init(port, nullptr, nullptr,
            FStreamContext(queue),
            WSS::Entry< FileStreamingService, WSS::ASYNC_REP >("fstream"));
    ws.StartLoop(1000, 
                 [stopService](){return !stopService;});
    return 0;
}


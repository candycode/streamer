#include <string>
#include <iostream>
#include <vector>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <condition_variable>

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
    friend class Executor; //to allow calls to Clear  
private:
    void Clear() { queue_.clear(); }    
private:
    std::deque< T > queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
};
//------------------------------------------------------------------------------
using File = vector< char >;
//note: make File a const File
using FileQueue = SyncQueue< shared_ptr< File > >;
using SessionToQueue = unordered_map< const void*, shared_ptr< FileQueue > >;
//------------------------------------------------------------------------------
class SessionQueues {
public:
    void Map(const void* user, int queueSize = -1 /*not used*/) {
        std::lock_guard< std::mutex > guard(mutex_);
        if(stoq_.find(user) != stoq_.end()) Remove(user);
        stoq_[user] = shared_ptr< FileQueue >(new FileQueue());
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
int main(int argc, char** argv) {
    if(argc != 5) {
        std::cout << "usage: " 
                  << argv[0]
                  << " <path> <prefix> <start frame #> <suffix>\n";
        return 1;
    }
    const string path = argv[1];
    const string prefix = argv[2];
    const string suffix = argv[4];
    const int startFrame = stoi(argv[3]); //throws if arg not valid
    const int maxSize = 100;
    
    FileReadService fs(maxSize, path, prefix, startFrame, suffix);
    
    BrokerService broker(ref(fs), ref(sendQueues));

    fs.Start();
    broker.Start();

    //Start file reading service, passing reference to queue to store files
    //queue has a max size so service blocks until queue is not full




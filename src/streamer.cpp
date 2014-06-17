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
///@todo consider adding a name filter (0001 -> 1) 
///and a content filter (double array, color map -> turbojpeg -> jpeg)
void ReadFileService(string path,
                     string prefix,
                     int startFrame,
                     const string& suffix,
                     bool& stopService,
                     SessionQueues& q) {
    if(path.size() < 1) throw logic_error("Invalid  path size");
    if(path[path.size()-1] != '/') path += '/';
    prefix = path + prefix;
    int retries = 5;//@todo: make it a parameter; use negative (-1) for infinite
    int throttleInterval = 1;
    int retryInterval = 2;
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
        ++startFrame;
        this_thread::sleep_for(chrono::seconds(throttleInterval));
    }
    if(retries == 0) throw logic_error("No file");
}
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


    //using FStreamContext = Context< SessionQueues >;
    //FStreamContext ctx;



    //Start file reading service, passing reference to queue to store files
    //queue has a max size so service blocks until queue is not full
}



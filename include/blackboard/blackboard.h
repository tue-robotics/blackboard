#ifndef BLACKBOARD_BLACKBOARD_H_
#define BLACKBOARD_BLACKBOARD_H_

#include "blackboard/key.h"
#include "blackboard/variant.h"
#include "blackboard/serializer.h"
#include "blackboard/buffer.h"

#include <vector>
#include <map>
#include <string>
#include <iostream>

// ROS
#include <ros/publisher.h>
#include <ros/subscriber.h>
#include <ros/node_handle.h>
#include <ros/callback_queue.h>

#include <blackboard/ValueUpdate.h>
#include <blackboard/KeyUpdate.h>

namespace bb
{

class Update;

// ----------------------------------------------------------------------------------------------------

class ROSRBytes : public RBytes
{
public:

    ROSRBytes(const blackboard::ValueUpdate& msg) : msg_(msg) {}

    const unsigned char* ptr() const { return &msg_.data[0]; }
    std::size_t size() const { return msg_.data.size(); }

private:

    const blackboard::ValueUpdate& msg_;

};

// ----------------------------------------------------------------------------------------------------

class ROSWBytes : public WBytes
{
public:

    ROSWBytes(blackboard::ValueUpdate& msg) : msg_(msg) {}

    unsigned char* ptr() { return &msg_.data[0]; }
    std::size_t size() const { return msg_.data.size(); }
    bool resize(std::size_t size) { msg_.data.resize(size); return true; }

private:

    blackboard::ValueUpdate& msg_;

};

// ----------------------------------------------------------------------------------------------------

class Blackboard;
typedef void (*trigger_function)(const Blackboard&, const Key& key);

// ----------------------------------------------------------------------------------------------------

struct Data
{
    Data() : serializer(0) {}

    std::vector<trigger_function> trigger_functions;
    Buffer<Variant> buffer;
    ros::Publisher pub;
    std::map<std::string, ros::Subscriber> subs;
    Serializer* serializer;
};

// ----------------------------------------------------------------------------------------------------

struct BlackboardConnection
{
    ros::Subscriber sub_keys;
};

// ----------------------------------------------------------------------------------------------------

class Blackboard
{

public:

    Blackboard()
    {
    }

    ~Blackboard()
    {

    }

    Key addKey(const char* name, Serializer* serializer = 0, unsigned long buffer_size = 0)
    {
        boost::upgrade_lock<boost::shared_mutex> lock(blackboard_mutex_);       // get upgradable access
        boost::upgrade_to_unique_lock<boost::shared_mutex> unique_lock(lock);   // get exclusive access

        std::map<std::string, Key>::iterator it = key_map_.find(name);
        if (it != key_map_.end())
            return it->second;

        Key key = key_map_.size();
        key_map_[name] = key;
        data_.push_back(Data());

        Data& d = data_.back();
        d.serializer = serializer;
        d.buffer.setMaxSize(buffer_size);

        ros::NodeHandle nh("~");
        d.pub = nh.advertise<blackboard::ValueUpdate>("bb/data/" + std::string(name), 1);

        sendKeys();

        return key;
    }

    Key findKey(const char* name) const
    {
        std::map<std::string, Key>::const_iterator it = key_map_.find(name);
        if (it != key_map_.end())
            return it->second;

        return -1;
    }

    void addTrigger(Key key, trigger_function func)
    {
        boost::upgrade_lock<boost::shared_mutex> lock(blackboard_mutex_);       // get upgradable access
        boost::upgrade_to_unique_lock<boost::shared_mutex> unique_lock(lock);   // get exclusive access

        data_[key].trigger_functions.push_back(func);
    }

    template<typename T>
    void setValue(Key key, Time t, T value)
    {
        {
            boost::upgrade_lock<boost::shared_mutex> lock(blackboard_mutex_);       // get upgradable access
            boost::upgrade_to_unique_lock<boost::shared_mutex> unique_lock(lock);   // get exclusive access

            Data& d = data_[key];

            // Set value
            d.buffer.insert(t, value);
        }

        checkTriggers(key, t, value);
    }

    template<typename T>
    const T* getValue(Key key, Time t) const
    {
        // get shared access
        boost::shared_lock<boost::shared_mutex> lock(blackboard_mutex_);

        const Data& d = data_[key];

        Buffer<Variant>::const_iterator lower, upper;
        d.buffer.getLowerUpper(t, lower, upper);
        if (lower != d.buffer.end())
            return &lower->second.getValue<T>();
        else if (upper != d.buffer.end())
            return &upper->second.getValue<T>();
        else
            return NULL;

    }

    void checkTriggers(Key key, Time t, const Variant& value);

//    void initCommunication(const std::string& name = "~");

    void sendKeys()
    {
        blackboard::KeyUpdate msg;

        for(std::map<std::string, Key>::const_iterator it = key_map_.begin(); it != key_map_.end(); ++it)
            msg.added_keys.push_back(it->first);

        pub_key_.publish(msg);
    }

    void initialize(const std::string& name = "~");

    void addExternal(const std::string& bb_id)
    {
        std::map<std::string, BlackboardConnection>::iterator it = connections_.find(bb_id);
        if (it != connections_.end())
            return;

        BlackboardConnection& c = connections_[bb_id];

        ros::NodeHandle nh;
        nh.setCallbackQueue(&key_cb_queue_);
        c.sub_keys = nh.subscribe<blackboard::KeyUpdate>(bb_id + "/bb/keys", 1, boost::bind(&Blackboard::cbKeyInfo, this, _1, bb_id));
    }

    void cbKeyInfo(const blackboard::KeyUpdateConstPtr& msg, const std::string& bb_id)
    {
        for(std::vector<std::string>::const_iterator it = msg->added_keys.begin(); it != msg->added_keys.end(); ++it)
        {
            Key key = findKey(it->c_str());
            if (key < 0)
                continue;

            Data& d = data_[key];
            std::map<std::string, ros::Subscriber>::iterator it_sub = d.subs.find(bb_id);
            if (it_sub != d.subs.end())
                continue;

            ros::NodeHandle nh;
            nh.setCallbackQueue(&value_cb_queue_);
            ros::Subscriber& sub = d.subs[bb_id];
            sub = nh.subscribe<blackboard::ValueUpdate>(bb_id + "/bb/data/" + *it, 1, boost::bind(&Blackboard::cbValue, this, _1, key));

        }
    }

    void cbValue(const blackboard::ValueUpdateConstPtr& msg, Key key)
    {
        Data& d = data_[key];
        if (!d.serializer)
        {
            std::cout << "No serializer for key " << key << std::endl;
            return;
        }

        Variant v;
        d.serializer->deserialize(ROSRBytes(*msg), v);
        d.buffer.insert(msg->timestamp, v);

        // Call triggers
        const std::vector<trigger_function>& trigger_functions = d.trigger_functions;
        for(std::vector<trigger_function>::const_iterator it = trigger_functions.begin(); it != trigger_functions.end(); ++it)
        {
            (*it)(*this, key);
        }
    }

    void update(const Update& update);

    void updateConnections()
    {
        key_cb_queue_.callAvailable();
    }

    void updateValues()
    {
        value_cb_queue_.callAvailable();
    }

private:

    // DATA

    std::map<std::string, Key> key_map_;

    std::vector<Data> data_;

    // Mutex for the full blackboard, so all data channels
    mutable boost::shared_mutex blackboard_mutex_;


    // COMMUNICATION

    ros::Publisher pub_key_;

    ros::CallbackQueue key_cb_queue_;

    ros::CallbackQueue value_cb_queue_;

    std::map<std::string, BlackboardConnection> connections_;

};

} // end namespace bb

#endif

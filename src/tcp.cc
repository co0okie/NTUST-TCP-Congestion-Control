#include <omnetpp.h>
#include <deque>
#include <string>
#include "tcp_m.h"

using namespace omnetpp;

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t i64;
typedef int32_t i32;
typedef int16_t i16;
typedef int8_t i8;

class LossyChannel : public cDatarateChannel {
  private:
    double droprate;
  public:
    void setDroprate(double droprate) {
        this->droprate = droprate;
    }
    double getDroprate() {
        return droprate;
    }
  protected:
    virtual Result processMessage(cMessage *msg, const SendOptions& options, simtime_t t) override {
        Result result = cDatarateChannel::processMessage(msg, options, t);
        if (uniform(0,1) < droprate) {
            result.discard = true;
            std::string dropMsg = std::string{msg->getName()} + " is dropped.\n";
            EV << dropMsg;
            bubble(dropMsg.c_str());
        }
        return result;
    }
    virtual void initialize() override {
        cDatarateChannel::initialize();
        droprate = par("droprate");
    }
};

Define_Channel(LossyChannel);

class TcpModule : public cSimpleModule {
  private:
    u32 mss;
    u32 seq;
    u32 seq_begin;
    u32 seq_end;
    u32 ack;
    u32 swnd;
    u32 rwnd;
    std::deque<u8> snd_buf;
    std::deque<u8> rcv_buf;
    std::map<u32, u32> rcv_ack; // <ack, size>
    double rto;
    cMessage* waitSend = new cMessage("waitSend");
    cMessage* rtoTimer = new cMessage("rtoTimer");
    void handleWaitSend(cMessage *msg);
    void handleRtoTimer(cMessage *msg);
    void handleTcpPacket(TcpPacket* msg);
    void handleAck(TcpPacket* msg);
  protected:
    virtual ~TcpModule() override;
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
};

Define_Module(TcpModule);

TcpModule::~TcpModule() {
    cancelAndDelete(waitSend);
    cancelAndDelete(rtoTimer);
}

void TcpModule::initialize() {
    mss = 2;
    swnd = 10;
    seq_begin = 0;
    seq_end = seq_begin + swnd;
    seq = seq_begin;
    rto = 5;
    ack = 0;
    rwnd = 100;
    rcv_buf.resize(rwnd);

    if (std::string(getName()) == "client") {
        for (int i = 0; i < 100; i++) {
            snd_buf.push_back(i);
        }
    }
    
    scheduleAt(0.0, waitSend);
}

void TcpModule::handleMessage(cMessage* msg) {
    if (msg == waitSend) {
        handleWaitSend(msg);
    }
    else if (msg == rtoTimer) {
        handleRtoTimer(msg);
    }
    else if (TcpPacket* tcpMsg = check_and_cast<TcpPacket*>(msg)) {
        handleTcpPacket(tcpMsg);
        delete msg;
    }
}

void TcpModule::handleWaitSend(cMessage* msg) {
    // nothing to send
    if (i32(seq_end - seq) <= 0 || i32(seq - seq_begin) >= snd_buf.size()) return;

    // channel busy
    auto* channel = gate("ip$o")->getTransmissionChannel();
    if (channel->isBusy()) {
        scheduleAt(channel->getTransmissionFinishTime(), waitSend);
        return;
    }

    auto* packet = new TcpPacket(("seq = " + std::to_string(seq)).c_str());
    packet->setSrc(1);
    packet->setDst(2);
    packet->setSeq(seq);
    packet->setAck(ack);
    packet->setSYN(false);
    packet->setFIN(false);
    packet->setACK(false);
    auto begin = snd_buf.begin() + u32(seq - seq_begin);
    auto size = std::min(mss, (seq_end - seq));
    packet->setPayloadArraySize(size);
    for (size_t i = 0; i < size; i++) {
        packet->setPayload(i, begin[i]);
    }
    packet->setByteLength(20 + size);
    send(packet, "ip$o");
    EV << "sending " << packet->getName();
    bubble((std::string{"sending "} + std::string{packet->getName()}).c_str());
    scheduleAt(channel->getTransmissionFinishTime(), waitSend);
    if (!rtoTimer->isScheduled()) scheduleAfter(rto, rtoTimer);
    seq += size;
}

void TcpModule::handleTcpPacket(TcpPacket* msg) {
    if (msg->getACK()) handleAck(msg);
    size_t payloadSize = msg->getPayloadArraySize();
    if (payloadSize && i32(msg->getSeq() - ack) < rwnd) {
        if (rcv_ack.find(msg->getSeq()) == rcv_ack.end()) {
            rcv_ack[msg->getSeq()] = payloadSize;
            for (
                size_t i0 = rcv_buf.size() - rwnd + msg->getSeq() - ack, i = 0; 
                i < payloadSize && i0 + i < rcv_buf.size(); i++
            ) rcv_buf[i0 + i] = msg->getPayload(i);
            if (auto it = rcv_ack.find(ack); it != rcv_ack.end()) {
                while (!rcv_ack.empty() && it->first <= ack) {
                    auto newAck = it->first + it->second;
                    rcv_buf.resize(rcv_buf.size() + i32(newAck - ack));
                    ack = newAck;
                    it = rcv_ack.erase(it);
                    if (it == rcv_ack.end()) it = rcv_ack.begin();
                }
            }
        }

        auto* ackMsg = new TcpPacket(("ack = " + std::to_string(ack)).c_str());
        ackMsg->setSrc(2);
        ackMsg->setDst(1);
        ackMsg->setSeq(seq);
        ackMsg->setAck(ack);
        ackMsg->setSYN(false);
        ackMsg->setFIN(false);
        ackMsg->setACK(true);
        ackMsg->setByteLength(20);
        send(ackMsg, "ip$o");
        EV << "sending " << ackMsg->getName();
        bubble((std::string{"receive "} + std::to_string(msg->getSeq()) 
            + ", require " + std::to_string(ack)).c_str());
    }
}


void TcpModule::handleAck(TcpPacket* msg) {
    // if seq is at position that is already ack
    if (i32(seq - msg->getAck()) < 0) {
        seq = msg->getAck();
    }
    i32 delSize = msg->getAck() - seq_begin;
    if (delSize <= 0) return;
    snd_buf.erase(snd_buf.begin(), snd_buf.begin() + std::min(size_t(delSize), snd_buf.size()));
    seq_begin += delSize;
    seq_end = seq + swnd;
    cancelEvent(rtoTimer);
    if (!waitSend->isScheduled()) scheduleAfter(0.0, waitSend);
    // if msg->getAck() != seq (not latest)
    if (delSize != 0) scheduleAfter(rto, rtoTimer);
}

void TcpModule::handleRtoTimer(cMessage* msg) {
    seq = seq_begin;
    if (!waitSend->isScheduled()) scheduleAfter(0.0, waitSend);
    EV << "Retransmission Timeout";
}
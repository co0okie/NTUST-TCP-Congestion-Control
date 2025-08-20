#include <omnetpp.h>
#include <deque>
#include <map>
#include <string>
#include "tcp_m.h"
#include "LossyChannel.h"

using std::deque, std::string, std::to_string, std::map;

using omnetpp::cSimpleModule, omnetpp::cMessage;

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t i64;
typedef int32_t i32;
typedef int16_t i16;
typedef int8_t i8;


class TcpModule : public cSimpleModule {
  private:
    u32 mss;
    u32 seq_begin;
    u32 seq;
    u32 swnd;
    deque<u8> snd_buf;
    double rto;

    u32 ack;
    bool ACK;
    u32 rwnd;
    deque<u8> rcv_buf;
    map<u32, u32> rcv_ack; // <ack, size>
    cMessage* waitSend = new cMessage("waitSend");
    cMessage* rtoTimer = new cMessage("rtoTimer");
    void handleWaitSend();
    void handleRtoTimer();
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
    mss = 100;
    swnd = 1000;
    seq_begin = 0;
    seq = seq_begin;
    rto = 1;
    ack = 0;
    ACK = false;
    rwnd = 1000;
    rcv_buf.resize(rwnd);

    if (string(getName()) == "client") {
        for (int i = 0; i < 12345; i++) {
            snd_buf.push_back(u8(i));
        }
    }
    
    handleWaitSend();
}

void TcpModule::handleMessage(cMessage* msg) {
    if (msg == waitSend) {
        handleWaitSend();
    }
    else if (msg == rtoTimer) {
        handleRtoTimer();
    }
    else if (TcpPacket* tcpMsg = check_and_cast<TcpPacket*>(msg)) {
        handleTcpPacket(tcpMsg);
        delete msg;
    }
}

void TcpModule::handleWaitSend() {
    const i32 sendableByteSize = std::max(0, 
        i32(seq_begin + std::min(swnd, u32(snd_buf.size())) - seq));
    const bool hasPayloadToSend = sendableByteSize > 0;
    // nothing to send
    if (!hasPayloadToSend && !ACK) return;

    // channel busy
    const auto* channel = gate("ip$o")->getTransmissionChannel();
    if (channel->isBusy()) {
        scheduleAt(channel->getTransmissionFinishTime(), waitSend);
        return;
    }

    const u32 payloadSize = std::min(mss, u32(sendableByteSize));
    const string packetName = "seq " + to_string(seq) + " ack " + to_string(ack) + 
        " ACK " + to_string(ACK) + " size " + to_string(payloadSize);
    auto* packet = new TcpPacket(packetName.c_str());
    packet->setSrc(1);
    packet->setDst(2);
    packet->setSeq(seq);
    packet->setAck(ack);
    packet->setSYN(false);
    packet->setFIN(false);
    packet->setACK(ACK);
    ACK = 0;
    if (hasPayloadToSend) {
        auto begin = snd_buf.begin() + u32(seq - seq_begin);
        packet->setPayloadArraySize(payloadSize);
        for (size_t i = 0; i < payloadSize; i++) {
            packet->setPayload(i, begin[i]);
        }
        seq += payloadSize;
        if (!rtoTimer->isScheduled()) scheduleAfter(rto, rtoTimer);
    }
    packet->setByteLength(20 + payloadSize);
    send(packet, "ip$o");
    EV << getName() << ": " << packet->getName();
    scheduleAt(channel->getTransmissionFinishTime(), waitSend);
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
    }
    if (payloadSize) {
        ACK = true;
        handleWaitSend();
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
    cancelEvent(rtoTimer);
    if (!waitSend->isScheduled()) scheduleAfter(0.0, waitSend);
    // if msg->getAck() != seq (not latest)
    if (delSize != 0) scheduleAfter(rto, rtoTimer);
}

void TcpModule::handleRtoTimer() {
    seq = seq_begin;
    if (!waitSend->isScheduled()) scheduleAfter(0.0, waitSend);
    EV << getName() << ": Retransmission Timeout";
}
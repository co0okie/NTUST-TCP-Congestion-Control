#include <omnetpp.h>
#include <deque>
#include <map>
#include <string>
#include <algorithm>
#include "TcpPacket_m.h"

using std::deque, std::string, std::to_string, std::map, std::pair;

using omnetpp::cSimpleModule, omnetpp::cMessage, omnetpp::simtime_t, 
    omnetpp::simTime;

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
    void updateRTO(double rtt);
    void updateCWND(u32 ackPayloadSize);
  protected:
    u32 mss;
    u32 seq_una;
    u32 snd_nxt;
    u32 rwnd;
    deque<u8> snd_buf;
    double srtt, rttvar;
    double rto;
    struct SendRecord {
        u32 begin_seq;
        u32 end_seq;
        simtime_t send_time;
        bool isResend;
    };
    deque<SendRecord> snd_record; // packet that is sent but not ack
    u32 cwnd;
    u32 ssthresh;

    u32 rcv_nxt;
    bool SYN;
    bool ACK;
    bool FIN;
    u32 rcv_wnd;
    deque<u8> rcv_buf;
    map<u32, u32> rcv_ack; // <ack, size>, record receiving packet

    cMessage* waitSend = new cMessage("waitSend");
    cMessage* rtoTimer = new cMessage("rtoTimer");

    void handleWaitSend();
    void handleRtoTimer();
    void handleTcpPacket(TcpPacket* msg);
    void handleAck(TcpPacket* msg);
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
    rwnd = 1000;
    seq_una = 0;
    snd_nxt = seq_una;
    srtt = rttvar = 0;
    rto = 1;
    cwnd = mss;
    ssthresh = 2000;
    rcv_nxt = 0;
    ACK = false;
    SYN = false;
    FIN = false;
    rcv_wnd = 10000;
    rcv_buf.resize(rcv_wnd);
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
    } else {
        EV << getName() << ": unknown message\n";
        delete msg;
    }
}

void TcpModule::updateRTO(double rtt) {
    if (srtt == 0) {
        srtt = rtt;
        rttvar = rtt / 2;
    } else {
        srtt = 0.875 * srtt + 0.125 * rtt;
        rttvar = 0.75 * rttvar + 0.25 * std::abs(srtt - rtt);
    }
    rto = srtt + 4 * rttvar;
}

void TcpModule::updateCWND(u32 ackPayloadSize) {
    if (cwnd < ssthresh) {
        cwnd += std::min(mss, ackPayloadSize);
    } else {
        cwnd += std::max(1u, mss * mss / cwnd);
    }
}


void TcpModule::handleWaitSend() {
    const u32 swnd = std::min(rwnd, cwnd);
    const i32 sendableByteSize = std::max(0, 
        i32(seq_una + std::min(swnd, u32(snd_buf.size())) - snd_nxt));
    const bool hasPayloadToSend = sendableByteSize > 0;
    // nothing to send
    if (!hasPayloadToSend && !ACK) return;

    // channel busy
    const auto* channel = gate("ip$o")->getTransmissionChannel();
    if (channel->isBusy()) {
        if (!waitSend->isScheduled()) scheduleAt(channel->getTransmissionFinishTime(), waitSend);
        return;
    }

    const u32 payloadSize = std::min(mss, u32(sendableByteSize));
    string packetName;
    if (hasPayloadToSend) 
        packetName = "seq [" + to_string(snd_nxt) + ", " + 
            to_string(snd_nxt + payloadSize) + ")";
    else 
        packetName = "ack " + to_string(rcv_nxt);
    auto* packet = new TcpPacket(packetName.c_str());
    packet->setSrc(1);
    packet->setDst(2);
    packet->setSeq(snd_nxt);
    packet->setAck(rcv_nxt);
    packet->setSYN(SYN);
    packet->setFIN(FIN);
    packet->setACK(ACK);
    packet->setWindow(rcv_wnd);
    if (hasPayloadToSend) {
        auto begin = snd_buf.begin() + u32(snd_nxt - seq_una);
        packet->setPayloadArraySize(payloadSize);
        for (size_t i = 0; i < payloadSize; i++) {
            packet->setPayload(i, begin[i]);
        }
        auto it = std::find_if(snd_record.begin(), snd_record.end(), 
            [&](const SendRecord& rec) { return i32(rec.end_seq - snd_nxt) > 0; });
        if (it == snd_record.end()) {
            snd_record.push_back({.begin_seq = snd_nxt, .end_seq = snd_nxt + payloadSize, 
                .send_time = simTime(), .isResend = false});
        } else {
            it->isResend = true;
        }
        snd_nxt += payloadSize;
        if (!rtoTimer->isScheduled()) scheduleAfter(rto, rtoTimer);
    }
    packet->setByteLength(20 + payloadSize);
    EV << getName() << ": send " << packet->getName() << "\n";
    send(packet, "ip$o");
    ACK = FIN = SYN = false;
    scheduleAt(channel->getTransmissionFinishTime(), waitSend);
}

void TcpModule::handleTcpPacket(TcpPacket* msg) {
    if (msg->getACK()) handleAck(msg);
    size_t payloadSize = msg->getPayloadArraySize();
    if (!payloadSize) {
        EV << getName() << ": no payload\n";
    }
    if (i32(msg->getSeq() - rcv_nxt) >= i32(rcv_wnd)) {
        EV << getName() << ": seq " << msg->getSeq() << " out of window\n";
    }
    if (i32(msg->getSeq() - rcv_nxt) < 0) {
        EV << getName() << ": seq " << msg->getSeq() << " already ack\n";
    }
    if (payloadSize && i32(msg->getSeq() - rcv_nxt) < i32(rcv_wnd) && i32(msg->getSeq() - rcv_nxt) >= 0) {
        EV << getName() << ": receive [" << msg->getSeq() << ", " << msg->getSeq() + payloadSize << ")\n";
        if (rcv_ack.find(msg->getSeq()) == rcv_ack.end()) {
            rcv_ack[msg->getSeq()] = payloadSize;
            for (
                size_t i0 = rcv_buf.size() - rcv_wnd + msg->getSeq() - rcv_nxt, i = 0; 
                i < payloadSize && i0 + i < rcv_buf.size(); i++
            ) rcv_buf[i0 + i] = msg->getPayload(i);
            if (auto it = rcv_ack.find(rcv_nxt); it != rcv_ack.end()) {
                while (!rcv_ack.empty() && it->first <= rcv_nxt) {
                    auto newAck = it->first + it->second;
                    rcv_buf.resize(rcv_buf.size() + i32(newAck - rcv_nxt));
                    rcv_nxt = newAck;
                    it = rcv_ack.erase(it);
                    if (it == rcv_ack.end()) it = rcv_ack.begin();
                }
            }
            //// simulate app taking data, delete if app is implemented
            EV << getName() << ": app consume [" << rcv_nxt + rcv_wnd - rcv_buf.size() 
                << ", " << rcv_nxt << ")\n";
            rcv_buf.erase(rcv_buf.begin(), rcv_buf.end() - rcv_wnd);
            //// -------------------------------------------------------
        } else {
            EV << getName() << ": seq " << msg->getSeq() << " already ack\n";
        }
    }
    if (payloadSize) {
        ACK = true;
        if (!waitSend->isScheduled()) scheduleAfter(0.0, waitSend);
    }
    rwnd = msg->getWindow();
}


void TcpModule::handleAck(TcpPacket* msg) {
    const auto msg_ack = msg->getAck();
    EV << getName() << ": receive ack " << msg_ack << "\n";
    // if seq is at position that is already ack
    if (i32(snd_nxt - msg_ack) < 0) {
        EV << getName() << ": seq " << snd_nxt << " already ack, stop retransmission\n";
        snd_nxt = msg_ack; // stop resending
    }
    i32 delSize = msg_ack - seq_una;
    if (delSize <= 0) {
        EV << getName() << ": received ack " << msg_ack << 
            " already delete, this might be a duplicate ack\n";
        return;
    }
    EV << getName() << ": delete [" << seq_una << ", " << msg_ack 
        << "), total size " << delSize << "\n";
    if (auto it = std::find_if(snd_record.begin(), snd_record.end(), 
    [&](const SendRecord& rec) { return rec.end_seq == msg_ack; });
    it != snd_record.end()) {
        if (!it->isResend) {
            updateRTO((simTime() - it->send_time).dbl());
            EV << getName() << ": ack " << msg_ack << " match [" << it->begin_seq <<
                ", " << it->end_seq << ") time " << it->send_time << " rtt " <<
                simTime() - it->send_time << " new rto " << rto << "\n";
        }
        updateCWND(it->end_seq - it->begin_seq);
        EV << getName() << ": cwnd " << cwnd << " ssthresh " << ssthresh << "\n";
    } else {
        EV << getName() << ": ack " << msg_ack << " not match\n";
    }
    snd_record.erase(snd_record.begin(), 
        std::find_if(snd_record.begin(), snd_record.end(), 
        [&](const SendRecord& rec) { return i32(rec.end_seq - msg_ack) > 0; })
    );
    snd_buf.erase(snd_buf.begin(), snd_buf.begin() + std::min(size_t(delSize), snd_buf.size()));
    seq_una += delSize;
    cancelEvent(rtoTimer);
    if (!waitSend->isScheduled()) scheduleAfter(0.0, waitSend);
    // if msg_ack != snd_nxt (not latest)
    if (delSize != 0) {
        EV << getName() << ": reschedule RTO\n";
        scheduleAfter(rto, rtoTimer);
    }
}

void TcpModule::handleRtoTimer() {
    EV << getName() << ": Retransmission Timeout";
    snd_nxt = seq_una;
    if (!waitSend->isScheduled()) scheduleAfter(0.0, waitSend);
    rto *= 2;
    if (rto > 60) rto = 60;
    EV << getName() << ": rto " << rto << "\n";

    for (auto& rec : snd_record) {
        rec.isResend = true;
    }

    // congestion avoidance
    ssthresh = cwnd / 2;
    cwnd = mss;
    EV << getName() << ": cwnd " << cwnd << " ssthresh " << ssthresh << "\n";
}

class ServerModule : public TcpModule {
  protected:
    virtual void initialize() override;
};

Define_Module(ServerModule);

void ServerModule::initialize() {
    TcpModule::initialize();
}

class ClientModule : public TcpModule {
  protected:
    virtual void initialize() override;
};

Define_Module(ClientModule);

void ClientModule::initialize() {
    TcpModule::initialize();
    for (int i = 0; i < 12345; i++) {
        snd_buf.push_back(intrand(256));
        
    }
    scheduleAfter(0.0, waitSend);
}
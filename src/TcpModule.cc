#include <omnetpp.h>
#include <deque>
#include <map>
#include <string>
#include <algorithm>
#include <sstream>
#include <functional>
#include "TcpPacket_m.h"

using std::deque, std::string, std::to_string, std::map, std::pair, std::stringstream;

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

// return true if x in [a, b)
template <typename T> bool within(T a, T x, T b) {
    return x - a < b - a;
}

class TcpModule : public cSimpleModule {
  private:
    struct RT {
        double srtt, rttvar, rto;
    } rt; // retransmission related status
    RT getNewRt(double rtt) const;
    u32 getNewCwnd(u32 ackPayloadSize) const;
    void handleWaitSend();
    void handleRtoTimer();
    void handleAck(TcpPacket* msg);
    void handleTcpPayload(TcpPacket* msg);
    u32 mss;
    u32 snd_una;
    u32 snd_nxt;
    u32 rwnd;
  protected:
    deque<u8> snd_buf;
  private:
    struct SendRecord {
        u32 begin_seq;
        u32 end_seq;
        simtime_t send_time;
    };
    deque<SendRecord> snd_record; // packet that is sent but not ack
    u32 cwnd;
    u32 ssthresh;
    bool rexmit; // whether retransmitting
    u32 rexmit_nxt; // next retransmitting seq, only valid if rexmit == true

    u32 rcv_nxt;
    bool ACK;
    u32 rcv_wnd;
    deque<u8> rcv_buf;
    std::function<bool(u32, u32)> rcv_seg_lt = [&](u32 a, u32 b) -> bool { 
        return within(rcv_nxt, a, b); 
    };
    map<u32, u32, decltype(rcv_seg_lt)> rcv_seg{rcv_seg_lt}; // <seq, size>, record receiving packet

  protected:
    cMessage* waitSend = new cMessage("waitSend");
    cMessage* rtoTimer = new cMessage("rtoTimer");
    virtual ~TcpModule() override;
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
    void appConsume(); // consume [rcv_user, rcv_nxt) in rcv_buf
};

Define_Module(TcpModule);

TcpModule::~TcpModule() {
    cancelAndDelete(waitSend);
    cancelAndDelete(rtoTimer);
}

void TcpModule::initialize() {
    mss = 1000;
    rwnd = 10000;
    snd_una = 0;
    snd_nxt = snd_una;
    rt.srtt = rt.rttvar = 0;
    rt.rto = 3;
    rexmit = false;
    cwnd = mss;
    ssthresh = 32 * 1024;
    rcv_nxt = 0;
    ACK = false;
    rcv_wnd = 1024 * 1024;
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
        handleAck(tcpMsg);
        handleTcpPayload(tcpMsg);
        rwnd = tcpMsg->getWindow();
        delete msg;
    } 
    else {
        EV << getName() << ": unknown message\n";
        delete msg;
    }
}

TcpModule::RT TcpModule::getNewRt(double rtt) const {
    RT newRt;
    if (rt.srtt == 0) {
        newRt.srtt = rtt;
        newRt.rttvar = rtt / 2;
    } else {
        newRt.srtt = 0.875 * rt.srtt + 0.125 * rtt;
        newRt.rttvar = 0.75 * rt.rttvar + 0.25 * std::abs(rt.srtt - rtt);
    }
    newRt.rto = std::min(60.0, std::max(1.0, newRt.srtt + 4 * newRt.rttvar));
    return newRt;
}

u32 TcpModule::getNewCwnd(u32 ackPayloadSize) const {
    if (cwnd < ssthresh) {
        return cwnd + std::min(mss, ackPayloadSize);
    } else {
        return cwnd + std::max(1u, mss * mss / cwnd);
    }
}

void TcpModule::handleWaitSend() {
    const auto seq = rexmit ? rexmit_nxt : snd_nxt;
    const u32 swnd = std::min(rwnd, cwnd);
    const u32 snd_end = snd_una + std::min(swnd, u32(snd_buf.size()));
    const u32 sendableByteSize = snd_end - seq;
    const bool hasPayloadToSend = sendableByteSize > 0;

    if (!hasPayloadToSend && !ACK) {
        EV << getName() << ": nothing to send\n";
        return;
    }

    // channel busy
    const auto* channel = gate("ip$o")->getTransmissionChannel();
    if (channel->isBusy()) {
        if (!waitSend->isScheduled()) scheduleAt(channel->getTransmissionFinishTime(), waitSend);
        return;
    }

    const u32 payloadSize = std::min(mss, u32(sendableByteSize));
    const u32 new_seq = seq + payloadSize;
    const string packetName = (hasPayloadToSend ?
        (stringstream{} << "seq [" << seq << ", " << seq + payloadSize << ") ").str() : ""
    ) + (ACK ? "ack " + to_string(rcv_nxt) : "");
    auto* packet = new TcpPacket(packetName.c_str());
    packet->setSeq(seq);
    packet->setAck(rcv_nxt);
    packet->setACK(ACK);
    packet->setWindow(rcv_wnd);
    if (hasPayloadToSend) {
        const auto begin = snd_buf.begin() + u32(seq - snd_una);
        packet->setPayloadArraySize(payloadSize);
        for (size_t i = 0; i < payloadSize; i++) {
            packet->setPayload(i, begin[i]);
        }
    }
    packet->setByteLength(20 + payloadSize);
    EV << getName() << ": send " << packet->getName() << "\n";
    send(packet, "ip$o");

    if (hasPayloadToSend && !rtoTimer->isScheduled()) 
        scheduleAfter(rt.rto, rtoTimer);
    if (within(snd_una, new_seq, snd_end)) 
        scheduleAt(channel->getTransmissionFinishTime(), waitSend);

    if (hasPayloadToSend && !rexmit) {
        snd_record.push_back({
            .begin_seq = snd_nxt, .end_seq = snd_nxt + payloadSize, 
            .send_time = simTime()
        });
    }
    // for (auto& rec : snd_record) {
    //     EV << getName() << ": [" << rec.begin_seq << ", " << rec.end_seq << ") time " << rec.send_time << " " << rec.isResend << "\n";
    // }
    if (rexmit) {
        if (within(snd_una, new_seq, snd_nxt)) {
            rexmit_nxt = new_seq;
        } else {
            EV << getName() << ": new seq " << new_seq << " reach SND.NXT " <<
                snd_nxt << ", exit retransmission\n";
            rexmit = false;
            snd_nxt = new_seq;
        }
    } else {
        snd_nxt = new_seq;
    }
    ACK = false;
}

void TcpModule::handleAck(TcpPacket* msg) {
    if (!msg->getACK()) return;
    const auto seg_ack = msg->getAck();
    EV << getName() << ": receive ack " << seg_ack << "\n";
    if (!within(snd_una, seg_ack - 1, snd_nxt)) {
        EV << getName() << ": ignore ack " << seg_ack << " not within range (" 
            << snd_una << ", " << snd_nxt << "]\n";
        return;
    }
    if (!waitSend->isScheduled()) scheduleAfter(0.0, waitSend);
    const u32 newAckSize = seg_ack - snd_una;
    EV << getName() << ": new ack range [" << snd_una << ", " << seg_ack << ")\n";
    const auto matchedRecord = std::find_if(snd_record.begin(), snd_record.end(), 
        [&](const SendRecord& rec) { return rec.end_seq == seg_ack; });
    const bool foundMatchedRecord = matchedRecord != snd_record.end();
    if (!foundMatchedRecord) {
        EV << getName() << ": ack " << seg_ack << " not match\n";
    }
    if (foundMatchedRecord) {
        const auto& r = matchedRecord;
        rt = getNewRt((simTime() - r->send_time).dbl());
        EV << getName() << ": ack " << seg_ack << " match [" << r->begin_seq <<
            ", " << r->end_seq << ") time " << r->send_time << " rtt " <<
            simTime() - r->send_time << " new rto " << rt.rto << "\n";
    }
    cancelEvent(rtoTimer);
    // exist unacknowledged bytes
    if (within(snd_una, seg_ack, snd_nxt)) {
        EV << getName() << ": reschedule RTO\n";
        scheduleAfter(rt.rto, rtoTimer);
    }

    cwnd = getNewCwnd(newAckSize);
    EV << getName() << ": cwnd " << cwnd << " ssthresh " << ssthresh << "\n";
    
    for (auto it = snd_record.begin(); 
    it != snd_record.end() && within(it->end_seq, seg_ack, snd_nxt);) {
        it = snd_record.erase(it);
    }
    // for (auto& rec : snd_record) {
    //     EV << getName() << ": [" << rec.begin_seq << ", " << rec.end_seq << ") time " << rec.send_time << " " << rec.isResend << "\n";
    // }
    snd_buf.erase(snd_buf.begin(), snd_buf.begin() + std::min(size_t(newAckSize), snd_buf.size()));
    if (rexmit) {
        if (seg_ack == snd_nxt) {
            EV << getName() << ": ack " << seg_ack << 
            " reach SND.NXT " << snd_nxt << ", exit retransmission\n";
            rexmit = false;
        } else if (within(snd_una, rexmit_nxt, seg_ack)) {
            EV << getName() << ": ack " << seg_ack << " > next retransmit seq " <<
                rexmit_nxt << ", skip to ack\n";
            rexmit_nxt = seg_ack;
        }
    }
    snd_una = seg_ack;
}

void TcpModule::handleTcpPayload(TcpPacket* msg) {
    const u32 seg_seq = msg->getSeq();
    const u32 seg_end = seg_seq + msg->getPayloadArraySize();
    const u32 rcv_last = rcv_nxt + rcv_wnd;
    const u32 rcv_user = rcv_last - rcv_buf.size();
    if (!(
        within(rcv_nxt, seg_seq, rcv_last) || 
        within(rcv_nxt, seg_end - 1u, rcv_last)
    )) {
        EV << getName() << ": receive [" << seg_seq << ", " << seg_end << 
            ") not overlap with window [" << rcv_nxt << ", " << rcv_nxt + rcv_wnd << ")\n";
        ACK = true;
        if (!waitSend->isScheduled()) scheduleAfter(0.0, waitSend);
        return;
    }
    const size_t validPayloadSize = std::min(msg->getPayloadArraySize(), 
        size_t(rcv_nxt + rcv_wnd - seg_seq));
    if (!validPayloadSize) {
        return;
    }
    EV << getName() << ": receive [" << msg->getSeq() << ", " << msg->getSeq() + validPayloadSize << ")\n";

    if (const auto existingSeg = rcv_seg.find(msg->getSeq()); 
    existingSeg == rcv_seg.end() || existingSeg->second < validPayloadSize) {
        rcv_seg[seg_seq] = validPayloadSize;
        for (size_t i0 = seg_seq - rcv_user, i = 0; i < validPayloadSize && i0 + i < rcv_buf.size(); i++) 
            rcv_buf[i0 + i] = msg->getPayload(i);
    } else {
        EV << getName() << ": [" << msg->getSeq() << ", " << msg->getSeq() + validPayloadSize << ") already exist\n";
    }
    auto new_rcv_nxt = rcv_nxt;
    for (auto it = rcv_seg.begin(); 
    it != rcv_seg.end() && within(it->first, new_rcv_nxt, rcv_last);) {
        new_rcv_nxt = it->first + it->second;
        it = rcv_seg.erase(it);
    }
    auto new_rcv_wnd = rcv_nxt + rcv_wnd - new_rcv_nxt;

    if (!rcv_seg.empty()) {
        EV << getName() << ": stored segments";
        for (auto& [seq, size] : rcv_seg) {
            EV << " [" << seq << ", " << seq + size << ")";
        }
        EV << "\n";
    }

    // EV << getName() << ": rcv_nxt " << rcv_nxt << "\n";
    // EV << getName() << ": rcv_wnd " << rcv_wnd << "\n";
    // EV << getName() << ": new_rcv_nxt " << new_rcv_nxt << "\n";
    // EV << getName() << ": new_rcv_wnd " << new_rcv_wnd << "\n";
    // for (auto& [k, v] : rcv_seg) {
    //     EV << getName() << ": [" << k << ", " << k + v << ")\n";
    // }

    rcv_nxt = new_rcv_nxt;
    rcv_wnd = new_rcv_wnd;
    ACK = true;
    if (!waitSend->isScheduled()) scheduleAfter(0.0, waitSend);

    //// simulate app consume data, delete if app is implemented
    appConsume();
    //// -------------------------------------------------------
}

void TcpModule::appConsume() {
    const u32 rcv_user = rcv_nxt + rcv_wnd - u32(rcv_buf.size());
    // nothing to consume
    if (within(rcv_nxt, rcv_user, rcv_nxt + rcv_wnd)) return;
    const auto consume_bytes = rcv_nxt - rcv_user;
    const auto new_rcv_wnd = rcv_wnd + consume_bytes;
    const auto rcv_buf_size = rcv_buf.size();
    EV << getName() << ": app consume [" << rcv_user << ", " << rcv_nxt << ")\n";
    rcv_buf.erase(rcv_buf.begin(), rcv_buf.begin() + consume_bytes);
    rcv_buf.resize(rcv_buf_size);
    rcv_wnd = new_rcv_wnd;
}

void TcpModule::handleRtoTimer() {
    const auto flightSize = snd_nxt - snd_una;
    EV << getName() << ": retransmission timeout\n";
    rexmit = true;
    rexmit_nxt = snd_una;
    if (!waitSend->isScheduled()) scheduleAfter(0.0, waitSend);
    rt.rto *= 2;
    if (rt.rto > 60) rt.rto = 60;
    EV << getName() << ": rto " << rt.rto << "\n";

    // not calculate RTT for retransmitted data
    snd_record.clear();

    // congestion avoidance
    ssthresh = std::max(flightSize / 2, 2 * mss);
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
    for (int i = 0; i < 256 * 1024; i++) {
        snd_buf.push_back(intrand(256));
        
    }
    scheduleAfter(0.0, waitSend);
}
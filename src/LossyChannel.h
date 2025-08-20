#pragma once
#include <omnetpp.h>

using omnetpp::cDatarateChannel, omnetpp::cMessage, omnetpp::SendOptions, 
    omnetpp::simtime_t;

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
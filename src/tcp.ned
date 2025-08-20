channel LossyChannel extends ned.DatarateChannel {
    parameters:
        double droprate = default(0);
    @class(LossyChannel);
}

channel Channel extends LossyChannel {
    datarate = 128kbps;
    delay = 200ms;
}

simple TcpModule {
    gates:
        inout ip;
}

network TcpNetwork {
    submodules:
        client: TcpModule;
        server: TcpModule;
    connections:
        client.ip <--> Channel { droprate = 0.3; } <--> server.ip;
}
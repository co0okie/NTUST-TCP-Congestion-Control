channel LossyChannel extends ned.DatarateChannel {
    parameters:
        double droprate = default(0);
    @class(LossyChannel);
}

channel Channel extends LossyChannel {
    datarate = 1000bps;
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
        client.ip <--> Channel { droprate = 0.5; } <--> server.ip;
}
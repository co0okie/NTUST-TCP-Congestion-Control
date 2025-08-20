channel RealisticChannel extends ned.DatarateChannel {
    parameters:
        double droprate = default(0);
    @class(RealisticChannel);
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
        client.ip <--> RealisticChannel { droprate = 0.3; } <--> server.ip;
}
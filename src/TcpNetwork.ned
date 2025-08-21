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

module ServerModule extends TcpModule {
    @class(ServerModule);
}

module ClientModule extends TcpModule {
    @class(ClientModule);
}

network TcpNetwork {
    submodules:
        client: ClientModule;
        server: ServerModule;
    connections:
        client.ip <--> RealisticChannel { droprate = 0.01; } <--> server.ip;
}
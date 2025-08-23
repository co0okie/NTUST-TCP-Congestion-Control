channel RealisticChannel extends ned.DatarateChannel {
    parameters:
        double droprate = default(0);
    @class(RealisticChannel);
    datarate = 1Mbps;
    delay = 1s;
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
        client: ClientModule {
            @display("p=41,59");
        }
        server: ServerModule {
            @display("p=99,59");
        }
    connections:
        client.ip <--> RealisticChannel { droprate = 0.01; } <--> server.ip;
}
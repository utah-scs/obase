package site.ycsb.db;

import java.net.*;
import java.nio.file.*;
import java.nio.channels.*;
import java.io.*;

public class CrestConnection {
    private SocketChannel socketChannel;
    private boolean isUnix = false;

    public CrestConnection(String host, int port, String unixSocketPath) throws IOException {
        if ("127.0.0.1".equals(host)) {
            this.isUnix = true;
            // For UNIX domain socket, use UnixDomainSocketAddress
            Path path = Path.of(unixSocketPath);
            UnixDomainSocketAddress address = UnixDomainSocketAddress.of(path);
            this.socketChannel = SocketChannel.open(StandardProtocolFamily.UNIX);
            this.socketChannel.connect(address);
        } else {
            // For TCP connection, use InetSocketAddress
            InetSocketAddress address = new InetSocketAddress(host, port);
            this.socketChannel = SocketChannel.open();
            this.socketChannel.connect(address);
        }
    }

    public SocketChannel getSocketChannel() {
        return this.socketChannel;
    }

    public void close() throws IOException {
        if (this.socketChannel != null) {
            this.socketChannel.close();
        }
    }
}

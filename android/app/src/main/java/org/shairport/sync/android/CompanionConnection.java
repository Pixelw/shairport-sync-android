// SPDX-License-Identifier: MIT
package org.shairport.sync.android;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import org.json.JSONObject;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ArrayBlockingQueue;

/** 独立 IO 线程：只连接本机 root SPS，队列有界且从不阻塞界面。 */
final class CompanionConnection implements Runnable {
    interface Listener {
        /** 接收完整播放快照。 */
        void state(JSONObject state);
        /** 接收有界封面数据。 */
        void cover(long generation, byte[] bytes);
        /** 通知本机连接中断。 */
        void disconnected();
        /** 接收控制请求回执。 */
        void result(JSONObject result);
    }
    private static final int MAGIC = 0x53505331;
    private final Listener listener;
    private final ArrayBlockingQueue<byte[]> commands = new ArrayBlockingQueue<>(8);
    private final Thread thread = new Thread(this, "SPS-companion-io");
    private volatile boolean stopped;
    private volatile LocalSocket socket;

    CompanionConnection(Listener listener) { this.listener = listener; }
    void start() { thread.start(); }
    boolean command(JSONObject value) {
        return socket != null && commands.offer(value.toString().getBytes(StandardCharsets.UTF_8));
    }
    void close() {
        stopped = true;
        LocalSocket current = socket;
        if (current != null) try { current.close(); } catch (IOException ignored) { }
        thread.interrupt();
    }

    /** 使用超时读取、连接关闭和退避等待实现可协作退出。 */
    @Override public void run() {
        int delay = 1000;
        while (!stopped) {
            LocalSocket local = new LocalSocket();
            socket = local;
            try {
                local.connect(new LocalSocketAddress("org.shairport.sync.android.companion.v1",
                        LocalSocketAddress.Namespace.ABSTRACT));
                if (local.getPeerCredentials().getUid() != 0) throw new IOException("SPS UID must be root");
                local.setSoTimeout(1000);
                DataInputStream in = new DataInputStream(local.getInputStream());
                DataOutputStream out = new DataOutputStream(local.getOutputStream());
                // 先收到有效快照后再接受控制，重连不重放旧命令。
                boolean ready = false;
                while (!stopped) {
                    if (ready) {
                        byte[] command;
                        while ((command = commands.poll()) != null) {
                            out.writeInt(MAGIC); out.writeInt(3); out.writeInt(command.length);
                            out.write(command); out.flush();
                        }
                    }
                    // 原生端每 500ms 发送快照，超时或半包均断开重连，避免协议失步。
                    if (in.readInt() != MAGIC) throw new IOException("Protocol version mismatch");
                    int type = in.readInt(), length = in.readInt();
                    if (length < 0 || length > 4 * 1024 * 1024 + 8 ||
                            ((type == 1 || type == 4) && length > 65536))
                        throw new IOException("Invalid frame length");
                    byte[] bytes = new byte[length]; in.readFully(bytes);
                    if (type == 1) {
                        listener.state(new JSONObject(new String(bytes, StandardCharsets.UTF_8)));
                        ready = true; delay = 1000;
                    } else if (type == 2 && length >= 8) {
                        java.nio.ByteBuffer buffer = java.nio.ByteBuffer.wrap(bytes);
                        long generation = buffer.getLong();
                        byte[] art = new byte[length - 8]; buffer.get(art);
                        listener.cover(generation, art);
                    } else if (type == 4) {
                        listener.result(new JSONObject(new String(bytes, StandardCharsets.UTF_8)));
                    } else throw new IOException("Unknown frame type");
                }
            } catch (Exception e) {
                if (!stopped) listener.disconnected();
            } finally {
                try { local.close(); } catch (IOException ignored) { }
                socket = null; commands.clear();
            }
            if (!stopped) try { Thread.sleep(delay); } catch (InterruptedException ignored) { }
            delay = Math.min(30000, delay * 2);
        }
    }
}

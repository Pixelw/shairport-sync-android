// SPDX-License-Identifier: MIT
package org.shairport.sync.android;

import android.media.AudioManager;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;
import android.util.Log;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;

final class SystemVolumeEndpoint implements AutoCloseable {
    private static final String NAME = "org.shairport.sync.android.volume.v1";
    private static final int MAGIC = 0x53505631;
    private static final int SCALE = 1_000_000;
    private static final int STREAM = AudioManager.STREAM_MUSIC;
    private final AudioManager audio;
    private final Thread worker = new Thread(this::serve, "SPS-system-volume");
    private boolean closed;
    private LocalServerSocket listener;
    private LocalSocket client;

    SystemVolumeEndpoint(AudioManager audio) {
        this.audio = audio;
    }

    void start() {
        worker.start();
    }

    /** 关闭监听和当前连接，唤醒音量线程。 */
    @Override public synchronized void close() {
        closed = true;
        if (client != null) {
            try { client.close(); } catch (IOException ignored) { }
        }
        if (listener != null) {
            try {
                // LocalServerSocket.close() 单独调用时可能无法唤醒阻塞的 accept。
                Os.shutdown(listener.getFileDescriptor(), OsConstants.SHUT_RDWR);
            } catch (ErrnoException ignored) { }
            try { listener.close(); } catch (IOException ignored) { }
        }
    }

    private void serve() {
        try (LocalServerSocket server = new LocalServerSocket(NAME)) {
            synchronized (this) {
                if (closed) return;
                listener = server;
            }
            while (true) {
                try (LocalSocket socket = server.accept()) {
                    synchronized (this) {
                        if (closed) return;
                        client = socket;
                    }
                    try {
                        handle(socket);
                    } catch (IOException ignored) {
                        // 客户端可能已达到截止时间。
                    } finally {
                        synchronized (this) { client = null; }
                    }
                }
            }
        } catch (IOException e) {
            synchronized (this) {
                if (!closed) Log.w("SPS-system-volume", "本机音量端点不可用", e);
            }
        } finally {
            synchronized (this) { listener = null; }
        }
    }

    private void handle(LocalSocket socket) throws IOException {
        if (socket.getPeerCredentials().getUid() != 0) return;
        socket.setSoTimeout(200);
        DataInputStream input = new DataInputStream(socket.getInputStream());
        if (input.readInt() != MAGIC) return;
        int request = input.readInt();
        if (request < -1 || request > SCALE) return;
        if (request >= 0) {
            // 放弃已断开连接的写请求，避免拥塞后执行过期调音。
            StructPollfd peer = new StructPollfd();
            peer.fd = socket.getFileDescriptor();
            try {
                Os.poll(new StructPollfd[] {peer}, 0);
            } catch (ErrnoException e) {
                throw new IOException(e);
            }
            if ((peer.revents & (OsConstants.POLLHUP | OsConstants.POLLERR | OsConstants.POLLNVAL)) != 0) return;
        }

        int result = -1;
        try {
            if (audio != null && !audio.isVolumeFixed()) {
                int max = audio.getStreamMaxVolume(STREAM);
                if (max > 0) {
                    int target = request > 0 ? Math.max(1, Math.round((float) request * max / SCALE)) : 0;
                    long deadline = SystemClock.uptimeMillis() + 120;
                    if (request == 0 && !audio.isStreamMute(STREAM)) {
                        // 某些设备的最小可听档位大于零，单独使用媒体流静音。
                        audio.adjustStreamVolume(STREAM, AudioManager.ADJUST_MUTE, 0);
                    } else if (request > 0) {
                        if (target != audio.getStreamVolume(STREAM)) {
                            audio.setStreamVolume(STREAM, target, 0);
                        }
                        if (audio.isStreamMute(STREAM)) {
                            audio.adjustStreamVolume(STREAM, AudioManager.ADJUST_UNMUTE, 0);
                        }
                    }
                    // adjustStreamVolume 可能异步生效，只在短时限内等待实际状态。
                    do {
                        int current = audio.getStreamVolume(STREAM);
                        boolean muted = audio.isStreamMute(STREAM);
                        if (current >= 0 && current <= max &&
                                (request < 0 || (request == 0 && (muted || current == 0)) ||
                                 (request > 0 && !muted && current > 0))) {
                            result = muted ? 0 : Math.round((float) current * SCALE / max);
                        } else {
                            result = -1;
                        }
                        if (request < 0 || (request == 0 && result == 0) ||
                                (request > 0 && !muted && current == target)) break;
                        if (SystemClock.uptimeMillis() >= deadline) break;
                        SystemClock.sleep(5);
                    } while (true);
                }
            }
        } catch (RuntimeException e) {
            Log.d("SPS-system-volume", "系统媒体音量不可用", e);
        }
        DataOutputStream output = new DataOutputStream(socket.getOutputStream());
        output.writeInt(MAGIC);
        output.writeInt(result);
    }
}

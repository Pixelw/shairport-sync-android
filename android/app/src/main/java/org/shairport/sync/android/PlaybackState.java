// SPDX-License-Identifier: MIT
package org.shairport.sync.android;

import org.json.JSONObject;

/** 当前快照的只读值，不包含任何发送端配对材料。 */
final class PlaybackState {
    final String session, state, title, artist, album, device;
    final long position, duration, cover;
    final int controls;
    // 0 表示投放已断开，-1 表示旧快照未提供连接号。
    final int connection;
    final boolean positionValid;
    final long receivedAt;

    PlaybackState(JSONObject value, long now) {
        session = value.optString("session"); state = value.optString("state", "stopped");
        title = value.optString("title"); artist = value.optString("artist");
        album = value.optString("album"); device = value.optString("device");
        position = Math.max(0, value.optLong("positionMs"));
        duration = Math.max(0, value.optLong("durationMs")); cover = value.optLong("cover");
        connection = value.optInt("connection", -1);
        controls = value.optInt("controls"); positionValid = value.optInt("positionValid") != 0;
        receivedAt = now;
    }

    long positionAt(long now, boolean connected) {
        long result = position;
        if (connected && positionValid && "playing".equals(state))
            result += Math.max(0, Math.min(1500, now - receivedAt));
        return duration > 0 ? Math.min(result, duration) : result;
    }
}

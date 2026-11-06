// SPDX-License-Identifier: MIT
package org.shairport.sync.android;

/** 与 Android 无关的会话状态机；所有调用限定在 Service 主线程。 */
final class SessionLifecycle {
    static final long EXIT_DELAY_MS = 20_000;
    String session = "";
    String dismissed = "";
    String opened = "";
    long deadline;
    boolean expired;

    boolean update(String nextSession, String state, long now) {
        boolean changedSession = !nextSession.isEmpty() && !nextSession.equals(session);
        if (changedSession) {
            session = nextSession;
            deadline = 0;
            expired = false;
        }
        if ("playing".equals(state) || "paused".equals(state) || "idle".equals(state)) {
            deadline = 0;
            expired = false;
        } else if (!session.isEmpty() && deadline == 0 && !expired) {
            deadline = now + EXIT_DELAY_MS;
        }
        boolean launch = "playing".equals(state) && !session.equals(dismissed) && !session.equals(opened);
        if (launch) opened = session;
        tick(now);
        return launch;
    }

    void disconnect(long now) {
        if (!session.isEmpty() && deadline == 0 && !expired) deadline = now + EXIT_DELAY_MS;
    }

    void tick(long now) {
        if (deadline != 0 && now >= deadline) { deadline = 0; expired = true; }
    }

    void dismiss() { dismissed = session; }
}

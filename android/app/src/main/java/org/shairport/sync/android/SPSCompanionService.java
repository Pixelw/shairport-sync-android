// SPDX-License-Identifier: MIT
package org.shairport.sync.android;

import android.app.*;
import android.content.*;
import android.graphics.*;
import android.os.*;
import android.provider.Settings;
import org.json.JSONObject;
import java.io.*;
import java.util.ArrayList;
import java.util.List;

/** 常驻桥接服务；生命周期与原生 SPS/NQPTP 完全独立。 */
public final class SPSCompanionService extends Service implements CompanionConnection.Listener {
    interface Observer { /** 播放状态或超时发生变化。 */ void changed(); }
    final class LocalBinder extends Binder { SPSCompanionService service() { return SPSCompanionService.this; } }
    private final Handler main = new Handler(Looper.getMainLooper());
    private final List<Observer> observers = new ArrayList<>();
    private final LocalBinder binder = new LocalBinder();
    private final SessionLifecycle lifecycle = new SessionLifecycle();
    private CompanionConnection connection;
    private SharedPreferences prefs;
    private PlaybackState state;
    private Bitmap artwork;
    private boolean connected;
    private volatile boolean destroyed;
    private long requestId, pendingId, pendingUntil;
    private String message = "";
    private String savedSnapshot = "{}";
    private long lastSaved;
    private final Runnable ticker = new Runnable() {
        /** 驱动单调时钟超时及界面进度，不持有唤醒锁。 */
        @Override public void run() {
            long before = lifecycle.deadline;
            lifecycle.tick(SystemClock.elapsedRealtime());
            if (before != lifecycle.deadline) save();
            if (pendingId != 0 && SystemClock.elapsedRealtime() >= pendingUntil) {
                pendingId = 0; message = "发送端未确认操作";
            }
            notifyObservers();
            main.postDelayed(this, 250);
        }
    };

    /** 建立通知后恢复会话标记并启动独立 IO 线程。 */
    @Override public void onCreate() {
        super.onCreate();
        NotificationManager manager = getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel("companion", "AirPlay 播放状态", NotificationManager.IMPORTANCE_LOW));
        startForeground(1, notification());
        prefs = getSharedPreferences("companion", MODE_PRIVATE);
        prefs.edit().putBoolean("enabled", true).apply();
        int boot = Settings.Global.getInt(getContentResolver(), Settings.Global.BOOT_COUNT, -1);
        if (boot == prefs.getInt("boot", -2)) {
            lifecycle.session = prefs.getString("session", "");
            // 系统重建可恢复播放页；手动关闭标记仍优先抑制弹出。
            lifecycle.opened = "";
            lifecycle.dismissed = prefs.getString("dismissed", "");
            lifecycle.deadline = prefs.getLong("deadline", 0);
            lifecycle.expired = prefs.getBoolean("expired", false);
            savedSnapshot = prefs.getString("snapshot", "{}");
            try { state = new PlaybackState(new JSONObject(savedSnapshot), SystemClock.elapsedRealtime()); }
            catch (Exception ignored) { }
            lifecycle.disconnect(SystemClock.elapsedRealtime());
        }
        prefs.edit().putInt("boot", boot).apply();
        lifecycle.tick(SystemClock.elapsedRealtime());
        connection = new CompanionConnection(this); connection.start();
        main.post(ticker);
    }

    /** 请求系统在资源允许时重建服务；不承诺强行停止后的恢复。 */
    @Override public int onStartCommand(Intent intent, int flags, int startId) { return START_STICKY; }
    /** 仅提供给同 App Activity 的本地绑定。 */
    @Override public IBinder onBind(Intent intent) { return binder; }
    /** 关闭 socket 唤醒 IO，并移除本服务注册的回调。 */
    @Override public void onDestroy() {
        destroyed = true; save(); connection.close(); main.removeCallbacksAndMessages(null); observers.clear();
        super.onDestroy();
    }

    private Notification notification() {
        PendingIntent open = PendingIntent.getActivity(this, 0,
                new Intent(this, PlaybackActivity.class).putExtra("automatic", false),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, "companion").setSmallIcon(org.shairport.sync.android.R.drawable.ic_notification)
                .setContentTitle("Shairport Sync Android").setContentText("正在监听本机 AirPlay 播放状态")
                .setOngoing(true).setContentIntent(open).setShowWhen(false).build();
    }

    private void save() {
        if (prefs == null) return;
        prefs.edit().putString("session", lifecycle.session).putString("opened", lifecycle.opened)
                .putString("dismissed", lifecycle.dismissed).putLong("deadline", lifecycle.deadline)
                .putBoolean("expired", lifecycle.expired).putString("snapshot", savedSnapshot).apply();
        lastSaved = SystemClock.elapsedRealtime();
    }
    private void notifyObservers() { for (Observer observer : new ArrayList<>(observers)) observer.changed(); }
    void observe(Observer observer) { observers.add(observer); observer.changed(); }
    void unobserve(Observer observer) { observers.remove(observer); }
    PlaybackState playback() { return state; }
    Bitmap artwork() { return artwork; }
    boolean connected() { return connected; }
    boolean expired() { return lifecycle.expired; }
    boolean pending() { return pendingId != 0; }
    String message() { return message; }
    void dismiss() { lifecycle.dismiss(); save(); }

    void command(String command) {
        if (state == null || !connected || pendingId != 0 || lifecycle.deadline != 0 || lifecycle.expired) return;
        try {
            long id = ++requestId;
            JSONObject value = new JSONObject().put("id", id).put("session", state.session).put("command", command);
            if (connection.command(value)) {
                pendingId = id; pendingUntil = SystemClock.elapsedRealtime() + 5000;
                message = "正在请求发送端…";
            } else message = "连接不可用";
        } catch (Exception ignored) { message = "无法发送操作"; }
        notifyObservers();
    }

    /** IO 线程收到快照后，仅在主线程更新会话与界面。 */
    @Override public void state(JSONObject value) {
        main.post(() -> {
            if (destroyed) return;
            PlaybackState next = new PlaybackState(value, SystemClock.elapsedRealtime());
            boolean same = state != null && state.session.equals(next.session);
            boolean terminal = "stopped".equals(next.state);
            // 结束后的空快照不覆盖最后歌曲；新会话则清除旧封面。
            if (!same || (!terminal && state != null && state.cover != next.cover)) {
                artwork = null;
            }
            boolean changed = !connected || !same || state == null || !state.state.equals(next.state);
            if (pendingId != 0 && state != null &&
                    (!state.state.equals(next.state) || !state.title.equals(next.title) || !same)) {
                pendingId = 0; message = "";
            }
            if (!terminal || !same || state == null) {
                state = next; savedSnapshot = value.toString();
            } else {
                try {
                    JSONObject retained = new JSONObject(savedSnapshot);
                    retained.put("state", "stopped").put("controls", 0);
                    state = new PlaybackState(retained, SystemClock.elapsedRealtime());
                    savedSnapshot = retained.toString();
                } catch (Exception ignored) { state = next; }
            }
            connected = true;
            if (changed && !terminal && pendingId == 0) message = "";
            if (!terminal && next.cover == 0) { artwork = null;  }
            boolean launch = lifecycle.update(next.session, next.state, SystemClock.elapsedRealtime());
            if (changed || launch || SystemClock.elapsedRealtime() - lastSaved >= 5000) save();
            if (launch && observers.isEmpty()) {
                Intent intent = new Intent(this, PlaybackActivity.class).putExtra("automatic", true)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
                startActivity(intent);
            }
            notifyObservers();
        });
    }

    /** 在 IO 线程按尺寸采样解码封面，避免主线程解码和超大位图。 */
    @Override public void cover(long generation, byte[] bytes) {
        Bitmap decoded = null;
        try {
            BitmapFactory.Options options = new BitmapFactory.Options(); options.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(bytes, 0, bytes.length, options);
            if (options.outWidth <= 0 || options.outHeight <= 0) return;
            options.inSampleSize = 1;
            while (Math.max(options.outWidth, options.outHeight) / options.inSampleSize > 768)
                options.inSampleSize *= 2;
            options.inJustDecodeBounds = false;
            options.inPreferredConfig = Bitmap.Config.RGB_565;
            decoded = BitmapFactory.decodeByteArray(bytes, 0, bytes.length, options);
        } catch (RuntimeException | OutOfMemoryError ignored) { }
        final Bitmap bitmap = decoded;
        main.post(() -> {
            if (destroyed) return;
            if (state != null && state.cover == generation && bitmap != null) {
                artwork = bitmap;  notifyObservers();
            }
        });
    }

    /** 通信中断只影响显示与控制，沿用已有退出截止时间。 */
    @Override public void disconnected() {
        main.post(() -> {
            if (destroyed) return;
            if (connected && state != null) {
                try {
                    JSONObject frozen = new JSONObject(savedSnapshot);
                    frozen.put("positionMs", state.positionAt(SystemClock.elapsedRealtime(), true));
                    state = new PlaybackState(frozen, SystemClock.elapsedRealtime());
                    savedSnapshot = frozen.toString();
                } catch (Exception ignored) { }
            }
            connected = false; pendingId = 0;
            message = "与 SPS 的连接已中断";
            lifecycle.disconnect(SystemClock.elapsedRealtime()); save(); notifyObservers();
        });
    }

    /** 请求回执只反映发送过程，播放状态仍等待发送端快照。 */
    @Override public void result(JSONObject value) {
        main.post(() -> {
            if (destroyed) return;
            if (value.optLong("id") != pendingId || pendingId == 0) return;
            if (!"dispatched".equals(value.optString("result"))) {
                pendingId = 0; message = "发送端不支持此操作或会话已结束";
            }
            notifyObservers();
        });
    }
}

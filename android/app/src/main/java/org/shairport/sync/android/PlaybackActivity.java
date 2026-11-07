// SPDX-License-Identifier: MIT
package org.shairport.sync.android;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.Shader;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.os.IBinder;
import android.os.SystemClock;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.TextUtils;
import android.text.style.TypefaceSpan;
import android.view.Gravity;
import android.view.View;
import android.view.ViewOutlineProvider;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

/**
 * 小屏横向播放页；只负责显示和用户操作，不持有原生服务。
 */
public final class PlaybackActivity extends Activity implements SPSCompanionService.Observer {
    private SPSCompanionService service;
    private boolean bound, automatic;
    private TextView device, title, artist, album, elapsed, remaining, status;
    private ImageView cover;
    private ImageButton previous, play, next;
    private ProgressBar progress;
    private Backdrop backdrop;
    private Bitmap lastArt;
    private Boolean lastPlaying;
    private final ServiceConnection binding = new ServiceConnection() {
        /** 绑定后订阅主线程状态。 */
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            service = ((SPSCompanionService.LocalBinder) binder).service();
            service.observe(PlaybackActivity.this);
        }

        /** 系统断开绑定后清除引用。 */
        @Override
        public void onServiceDisconnected(ComponentName name) {
            service = null;
        }
    };

    /**
     * 建立原生 View 布局并开启前台监听。
     */
    @Override
    public void onCreate(Bundle saved) {
        super.onCreate(saved);
        automatic = saved != null ? saved.getBoolean("automatic") : getIntent().getBooleanExtra("automatic", false);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN |
                WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON |
                WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED |
                WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        buildLayout();
        startForegroundService(new Intent(this, SPSCompanionService.class));
    }

    /**
     * 保留页面来源，确保旋转或重建不会改变退出行为。
     */
    @Override
    protected void onSaveInstanceState(Bundle out) {
        out.putBoolean("automatic", automatic);
        super.onSaveInstanceState(out);
    }

    /**
     * 图标或通知打开已存在页面时，将其切换为手动模式。
     */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        automatic = intent.getBooleanExtra("automatic", false);
    }

    /**
     * 可见时订阅服务。
     */
    @Override
    protected void onStart() {
        super.onStart();
        bound = bindService(new Intent(this, SPSCompanionService.class), binding, BIND_AUTO_CREATE);
    }

    /**
     * 页面不可见时解除订阅，后台监听继续运行。
     */
    @Override
    protected void onStop() {
        if (service != null) service.unobserve(this);
        if (bound) unbindService(binding);
        bound = false;
        service = null;
        super.onStop();
    }

    /**
     * 用户主动返回立即退出，并抑制同一会话再次弹出。
     */
    @Override
    public void onBackPressed() {
        if (service != null) service.dismiss();
        finishAndRemoveTask();
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private TextView text(int size, int color) {
        TextView v = new TextView(this);
        v.setTextColor(color);
        v.setTextSize(size);
        v.setTypeface(getResources().getFont(R.font.inter_regular));
        v.setFontFeatureSettings("tnum");
        v.setEllipsize(TextUtils.TruncateAt.END);
        v.setMaxLines(1);
        return v;
    }

    private LinearLayout.LayoutParams row(int height) {
        return new LinearLayout.LayoutParams(-1, dp(height));
    }

    private ImageButton control(String description, int kind, String command) {
        ImageButton button = new ImageButton(this);
        button.setImageDrawable(new TransportIcon(kind));
        button.setBackgroundColor(Color.TRANSPARENT);
        button.setContentDescription(description);
        button.setPadding(dp(12), dp(12), dp(12), dp(12));
        button.setOnClickListener(v -> {
            if (service != null) service.command(command);
        });
        return button;
    }

    private void buildLayout() {
        FrameLayout root = new FrameLayout(this);
        backdrop = new Backdrop(this);
        root.addView(backdrop, new FrameLayout.LayoutParams(-1, -1));
        LinearLayout content = new LinearLayout(this);
        content.setGravity(Gravity.CENTER_VERTICAL);
        content.setPadding(dp(28), dp(24), dp(28), dp(24));
        root.addView(content, new FrameLayout.LayoutParams(-1, -1));
        FrameLayout artworkBox = new FrameLayout(this);
        content.addView(artworkBox, new LinearLayout.LayoutParams(0, -1, 0.46f));
        cover = new ImageView(this);
        cover.setScaleType(ImageView.ScaleType.FIT_CENTER);
        cover.setContentDescription("专辑封面");
        cover.setImageResource(R.drawable.ic_notification);
        cover.setElevation(dp(8));
        cover.setOutlineProvider(new ViewOutlineProvider() {
            /** 封面采用圆角外轮廓。 */
            @Override
            public void getOutline(View view, Outline outline) {
                outline.setRoundRect(0, 0, view.getWidth(), view.getHeight(), dp(18));
            }
        });
        cover.setClipToOutline(true);
        FrameLayout.LayoutParams artParams = new FrameLayout.LayoutParams(-1, -1, Gravity.CENTER_VERTICAL | Gravity.START);
        artParams.setMargins(0, dp(12), dp(26), dp(12));
        artworkBox.addView(cover, artParams);
        artworkBox.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            int size = Math.min(v.getWidth() - dp(26), v.getHeight() - dp(24));
            if (size > 0 && (cover.getLayoutParams().width != size || cover.getLayoutParams().height != size)) {
                FrameLayout.LayoutParams params = (FrameLayout.LayoutParams) cover.getLayoutParams();
                params.width = params.height = size;
                cover.setLayoutParams(params);
            }
        });
        LinearLayout details = new LinearLayout(this);
        details.setOrientation(LinearLayout.VERTICAL);
        content.addView(details, new LinearLayout.LayoutParams(0, -1, 0.54f));
        device = text(13, 0xCCFFFFFF);
        label(device, getString(R.string.waiting_device));
        details.addView(device, row(28));
        View space = new View(this);
        details.addView(space, new LinearLayout.LayoutParams(1, 0, 1));
        title = text(28, Color.WHITE);
        title.setTypeface(getResources().getFont(R.font.inter_bold));
        title.setMaxLines(2);
        label(title, getString(R.string.waiting_title));
        details.addView(title, new LinearLayout.LayoutParams(-1, -2));
        artist = text(19, 0xDDFFFFFF);
        details.addView(artist, row(30));
        album = text(14, 0xAAFFFFFF);
        details.addView(album, row(24));
        details.addView(new View(this), row(8));
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(10000);
        progress.setProgressTintList(android.content.res.ColorStateList.valueOf(Color.WHITE));
        progress.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(0x40FFFFFF));
        progress.setContentDescription("播放进度，只读");
        details.addView(progress, row(6));
        LinearLayout times = new LinearLayout(this);
        elapsed = text(12, 0xBBFFFFFF);
        remaining = text(12, 0xBBFFFFFF);
        remaining.setGravity(Gravity.END);
        times.addView(elapsed, new LinearLayout.LayoutParams(0, dp(24), 1));
        times.addView(remaining, new LinearLayout.LayoutParams(0, dp(24), 1));
        details.addView(times);
        LinearLayout controls = new LinearLayout(this);
        controls.setGravity(Gravity.CENTER);
        previous = control("上一首", 0, "previous");
        play = control("播放", 1, "play");
        next = control("下一首", 3, "next");
        controls.addView(previous, new LinearLayout.LayoutParams(0, dp(56), 1));
        controls.addView(play, new LinearLayout.LayoutParams(0, dp(64), 1.2f));
        controls.addView(next, new LinearLayout.LayoutParams(0, dp(56), 1));
        details.addView(controls);
        status = text(11, 0xAAFFFFFF);
        details.addView(status, row(18));
        details.addView(new View(this), new LinearLayout.LayoutParams(1, 0, 0.3f));
        setContentView(root);
    }

    private String fallback(String value, String fallback) {
        return value.isEmpty() ? fallback : value;
    }

    private String time(long milliseconds) {
        long seconds = milliseconds / 1000;
        return seconds >= 3600 ? String.format(java.util.Locale.ROOT, "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60) :
                String.format(java.util.Locale.ROOT, "%d:%02d", seconds / 60, seconds % 60);
    }

    private void enable(ImageButton button, boolean enabled) {
        if (button.isEnabled() != enabled) {
            button.setEnabled(enabled);
            button.setAlpha(enabled ? 1 : 0.3f);
        }
    }

    private void label(TextView view, String value) {
        if (TextUtils.equals(view.getText(), value)) return;
        SpannableString text = new SpannableString(value);
        java.util.regex.Matcher han = PlayerFonts.HAN.matcher(value);
        while (han.find()) {
            text.setSpan(new TypefaceSpan("sans-serif"), han.start(), han.end(),
                    Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        view.setText(text);
    }

    /**
     * 使用真实快照渲染，不因按钮点击提前改变播放状态。
     */
    @Override
    public void changed() {
        if (service == null) return;
        PlaybackState state = service.playback();
        if (service.expired() && automatic) {
            finishAndRemoveTask();
            return;
        }
        boolean waiting = state == null || service.expired() || state.session.isEmpty();
        boolean online = service.connected();
        boolean stopped = waiting || !online || "stopped".equals(state.state);
        label(title, waiting ? "等待 AirPlay" : fallback(state.title, "未知歌曲"));
        label(artist, waiting ? "Shairport Sync Android" : fallback(state.artist, "未知艺术家"));
        label(album, waiting ? "" : fallback(state.album, "未知专辑"));
        label(device, waiting ? "AIRPLAY · 等待投放" : "AIRPLAY · " + fallback(state.device, "未知投放设备"));
        long position = waiting ? 0 : state.positionAt(SystemClock.elapsedRealtime(), online);
        long duration = waiting ? 0 : state.duration;
        progress.setProgress(duration > 0 ? (int) Math.min(10000, position * 10000 / duration) : 0);
        label(elapsed, !waiting && state.positionValid ? time(position) : "—:—");
        label(remaining, duration > 0 ? "−" + time(Math.max(0, duration - position)) : "—:—");
        boolean playing = !waiting && "playing".equals(state.state);
        if (lastPlaying == null || lastPlaying != playing) {
            lastPlaying = playing;
            play.setImageDrawable(new TransportIcon(playing ? 2 : 1));
            play.setContentDescription(playing ? "暂停" : "播放");
            play.setOnClickListener(v -> {
                if (service != null) service.command(playing ? "pause" : "play");
            });
        }
        int mask = waiting ? 0 : state.controls;
        boolean usable = !stopped && !service.pending();
        enable(previous, usable && (mask & 32) != 0);
        enable(next, usable && (mask & 16) != 0);
        enable(play, usable && (mask & (playing ? 2 : 1)) != 0);
        label(status, !online ? "正在连接本机 SPS…" : stopped && !waiting ? "播放已结束" :
                !service.message().isEmpty() ? service.message() : waiting ? "准备就绪" : playing ? "正在播放" : "已暂停");
        Bitmap art = waiting ? null : service.artwork();
        if (art != lastArt) {
            lastArt = art;
            if (art == null) cover.setImageResource(R.drawable.ic_notification);
            else cover.setImageBitmap(art);
            backdrop.artwork(art);
        }
    }

    /**
     * API27 可用的低分辨率模糊封面背景，不依赖 RenderEffect。
     */
    private static final class Backdrop extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private Bitmap image;
        private final Rect destination = new Rect();
        private LinearGradient shade;

        Backdrop(Context context) {
            super(context);
        }

        void artwork(Bitmap art) {
            if (image != null) image.recycle();
            image = null;
            if (art != null) {
                Bitmap small = Bitmap.createScaledBitmap(art, 24, 24, true);
                int[] pixels = new int[576], blurred = new int[576];
                small.getPixels(pixels, 0, 24, 0, 0, 24, 24);
                for (int y = 0; y < 24; y++)
                    for (int x = 0; x < 24; x++) {
                        int red = 0, green = 0, blue = 0, count = 0;
                        for (int dy = -3; dy <= 3; dy++)
                            for (int dx = -3; dx <= 3; dx++) {
                                int color = pixels[Math.max(0, Math.min(23, y + dy)) * 24 + Math.max(0, Math.min(23, x + dx))];
                                red += Color.red(color);
                                green += Color.green(color);
                                blue += Color.blue(color);
                                count++;
                            }
                        blurred[y * 24 + x] = Color.rgb(red / count, green / count, blue / count);
                    }
                image = Bitmap.createBitmap(blurred, 24, 24, Bitmap.Config.RGB_565);
                small.recycle();
            }
            invalidate();
        }

        /**
         * 尺寸变化时预计算绘图资源，避免逐帧分配。
         */
        @Override
        protected void onSizeChanged(int w, int h, int oldw, int oldh) {
            destination.set(0, 0, w, h);
            shade = new LinearGradient(0, 0, w, h, 0x80303C46, 0xDD0B1118, Shader.TileMode.CLAMP);
        }

        /**
         * 拉伸模糊缩略图后叠加暗色渐变。
         */
        @Override
        protected void onDraw(Canvas canvas) {
            canvas.drawColor(0xFF263945);
            paint.setShader(null);
            if (image != null) canvas.drawBitmap(image, null, destination, paint);
            paint.setShader(shade);
            canvas.drawRect(0, 0, getWidth(), getHeight(), paint);
            paint.setShader(null);
        }
    }

    private static final class TransportIcon extends Drawable {
        private final int kind;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

        TransportIcon(int kind) {
            this.kind = kind;
            paint.setColor(Color.WHITE);
        }

        /**
         * 使用矢量路径绘制控制图标，避免字体缺字。
         */
        @Override
        public void draw(Canvas canvas) {
            canvas.save();
            canvas.translate(getBounds().left, getBounds().top);
            canvas.scale(getBounds().width() / 48f, getBounds().height() / 48f);
            if (kind == 2) {
                canvas.drawRoundRect(13, 8, 21, 40, 2, 2, paint);
                canvas.drawRoundRect(28, 8, 36, 40, 2, 2, paint);
            } else if (kind == 1) triangle(canvas, 16, 8, 38, 24, 16, 40);
            else {
                if (kind == 0) {
                    canvas.translate(48, 0);
                    canvas.scale(-1, 1);
                }
                triangle(canvas, 7, 11, 25, 24, 7, 37);
                triangle(canvas, 24, 11, 42, 24, 24, 37);
            }
            canvas.restore();
        }

        private void triangle(Canvas c, float x, float y, float x2, float y2, float x3, float y3) {
            Path path = new Path();
            path.moveTo(x, y);
            path.lineTo(x2, y2);
            path.lineTo(x3, y3);
            path.close();
            c.drawPath(path, paint);
        }

        /**
         * 更新系统要求的图标透明度。
         */
        @Override
        public void setAlpha(int alpha) {
            paint.setAlpha(alpha);
        }

        /**
         * 接受系统颜色滤镜。
         */
        @Override
        public void setColorFilter(ColorFilter filter) {
            paint.setColorFilter(filter);
        }

        /**
         * 图标含透明区域。
         */
        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }
}

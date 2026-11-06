// SPDX-License-Identifier: MIT
package org.shairport.sync.android

import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.ServiceConnection
import android.graphics.Bitmap
import android.os.Bundle
import android.os.IBinder
import android.os.SystemClock
import android.text.format.DateFormat
import android.view.View
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.compose.setContent
import androidx.core.graphics.scale
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.basicMarquee
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicText
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.changedToUpIgnoreConsumed
import androidx.compose.ui.input.pointer.changedToDownIgnoreConsumed
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.*
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.util.Date
import java.util.Locale
import kotlin.math.exp
import kotlin.math.roundToInt

/** Compose 播放页：显示本机服务快照并转发用户操作，不管理原生播放进程。 */
class PlayerV2Activity : ComponentActivity(), SPSCompanionService.Observer {
    private var service: SPSCompanionService? = null
    private var bound = false
    private var automatic = false
    private var sessionObserved = false
    private var screen by mutableStateOf(PlayerSnapshot())
    private val binding = object : ServiceConnection {
        /** 绑定成功后订阅服务主线程快照。 */
        override fun onServiceConnected(name: ComponentName, binder: IBinder) {
            service = (binder as SPSCompanionService.LocalBinder).service()
            service?.observe(this@PlayerV2Activity)
        }

        /** 绑定丢失时立即禁用控制并保留最后显示信息。 */
        override fun onServiceDisconnected(name: ComponentName) {
            service?.unobserve(this@PlayerV2Activity)
            service = null
            screen = screen.copy(previous = false, transport = false, next = false,
                message = "与 SPS 的连接已中断")
        }
    }

    /** 建立沉浸式 Compose 页面，恢复打开来源并启动后台监听。 */
    @Suppress("DEPRECATION")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        automatic = savedInstanceState?.getBoolean("automatic")
            ?: intent.getBooleanExtra("automatic", false)
        sessionObserved = savedInstanceState?.getBoolean("sessionObserved") ?: false
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN or
            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON or
            WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED or
            WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON)
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            /** 主动关闭时沿用当前会话的再次弹出抑制规则。 */
            override fun handleOnBackPressed() {
                service?.dismiss()
                finishAndRemoveTask()
            }
        })
        setContent { PlayerScreen(screen) { service?.command(it) } }
        startForegroundService(Intent(this, SPSCompanionService::class.java))
    }

    /** 保存打开来源及本页会话标记，使页面重建保持会话结束后的退出规则。 */
    override fun onSaveInstanceState(outState: Bundle) {
        outState.putBoolean("automatic", automatic)
        outState.putBoolean("sessionObserved", sessionObserved)
        super.onSaveInstanceState(outState)
    }

    /** 应用图标和通知重用页面时更新为对应的打开来源。 */
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        automatic = intent.getBooleanExtra("automatic", false)
        changed()
    }

    /** 页面可见时绑定并订阅后台监听服务。 */
    override fun onStart() {
        super.onStart()
        bound = bindService(Intent(this, SPSCompanionService::class.java), binding, BIND_AUTO_CREATE)
    }

    /** 页面不可见时解除订阅和绑定，后台播放不受影响。 */
    override fun onStop() {
        service?.unobserve(this)
        if (bound) unbindService(binding)
        bound = false
        service = null
        super.onStop()
    }

    /** 将真实服务状态转换为不可变显示快照，不预测命令执行结果。 */
    override fun changed() {
        val current = service ?: return
        val state = current.playback()
        val expired = current.expired()
        if (expired && (automatic || sessionObserved)) {
            finish()
            return
        }
        // 会话结束前或宽限期内已打开的页面都应退出；结束后新开的等待页不受旧超时影响。
        if (!expired && state != null && state.session.isNotEmpty()) sessionObserved = true
        val waiting = state == null || expired || state.session.isEmpty()
        val online = current.connected()
        val stopped = waiting || !online || state?.state == "stopped"
        val playing = !waiting && state?.state == "playing"
        val mask = if (waiting) 0 else state?.controls ?: 0
        val usable = !stopped && !current.pending()
        screen = PlayerSnapshot(
            title = if (waiting) "等待 AirPlay" else state!!.title.ifEmpty { "未知歌曲" },
            artist = if (waiting) "Shairport Sync Android" else state!!.artist.ifEmpty { "未知艺术家" },
            artwork = if (waiting) null else current.artwork(),
            position = if (waiting) 0 else state!!.positionAt(SystemClock.elapsedRealtime(), online),
            duration = if (waiting) 0 else state!!.duration,
            positionValid = !waiting && state!!.positionValid,
            playing = playing,
            previous = usable && mask and 32 != 0,
            transport = usable && mask and (if (playing) 2 else 1) != 0,
            next = usable && mask and 16 != 0,
            message = when {
                !online -> "正在连接本机 SPS…"
                current.message().isNotEmpty() -> current.message()
                stopped && !waiting -> "播放已结束"
                waiting -> "准备就绪"
                else -> ""
            }
        )
    }
}

private data class PlayerSnapshot(
    val title: String = "等待 AirPlay",
    val artist: String = "Shairport Sync Android",
    val artwork: Bitmap? = null,
    val position: Long = 0,
    val duration: Long = 0,
    val positionValid: Boolean = false,
    val playing: Boolean = false,
    val previous: Boolean = false,
    val transport: Boolean = false,
    val next: Boolean = false,
    val message: String = "正在连接本机 SPS…"
)

@Composable
private fun PlayerScreen(state: PlayerSnapshot, command: (String) -> Unit) {
    var controls by rememberSaveable { mutableStateOf(false) }
    var lastTouch by rememberSaveable { mutableLongStateOf(0L) }
    val blurred by produceState<Bitmap?>(null, state.artwork) {
        value = state.artwork?.let { art -> withContext(Dispatchers.Default) { gaussianBackdrop(art) } }
    }
    LaunchedEffect(controls, lastTouch) {
        if (controls) {
            delay((20_000 - (SystemClock.elapsedRealtime() - lastTouch)).coerceIn(0, 20_000))
            controls = false
        }
    }
    val clock = currentTimeText()
    val interaction = { lastTouch = SystemClock.elapsedRealtime() }
    val toggleControls = {
        controls = !controls
        interaction()
    }
    Box(Modifier.fillMaxSize().background(Color(0xFF263945)).pointerInput(controls) {
        awaitPointerEventScope {
            while (true) {
                val event = awaitPointerEvent(PointerEventPass.Initial)
                if (controls && event.changes.any { it.changedToDownIgnoreConsumed() || it.changedToUpIgnoreConsumed() }) interaction()
            }
        }
    }.clickable(
        interactionSource = remember { MutableInteractionSource() }, indication = null,
        onClickLabel = if (controls) "隐藏播放控件" else "显示播放控件",
        onClick = toggleControls
    )) {
        blurred?.let {
            Image(it.asImageBitmap(), null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
        }
        Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = .18f)))
        // 子按钮会消费点击（禁用时也消费），背景只处理非按钮区域的显隐切换。
        PlayerLayout(state, controls, clock) {
            interaction()
            command(it)
        }
        // 展示态的透明触摸层吞掉首次唤出手势，避免点到尚未显示的播放按钮。
        if (!controls) Box(Modifier.fillMaxSize().clickable(
            interactionSource = remember { MutableInteractionSource() }, indication = null,
            onClickLabel = "显示播放控件", role = Role.Button, onClick = toggleControls
        ).semantics { contentDescription = "$clock，${state.title} · ${state.artist}，显示播放控件" })
    }
}

@Composable
private fun currentTimeText(): String {
    val context = androidx.compose.ui.platform.LocalContext.current
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    DisposableEffect(context) {
        val receiver = object : BroadcastReceiver() {
            /** 系统时间和时区变化后立即刷新时钟。 */
            override fun onReceive(context: Context?, intent: Intent?) { now = System.currentTimeMillis() }
        }
        context.registerReceiver(receiver, IntentFilter().apply {
            addAction(Intent.ACTION_TIME_TICK)
            addAction(Intent.ACTION_TIME_CHANGED)
            addAction(Intent.ACTION_TIMEZONE_CHANGED)
        })
        onDispose { context.unregisterReceiver(receiver) }
    }
    LaunchedEffect(Unit) {
        while (true) {
            now = System.currentTimeMillis()
            delay(1_000)
        }
    }
    return DateFormat.getTimeFormat(context).format(Date(now))
}

@Composable
private fun PlayerLayout(state: PlayerSnapshot, controls: Boolean, clock: String, command: (String) -> Unit) {
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val coverSize by animateDpAsState(
            minOf((maxWidth - 48.dp).coerceAtLeast(48.dp), (maxHeight - 48.dp - if (controls) 24.dp else 80.dp).coerceAtLeast(48.dp)),
            tween(250), label = "coverSize")
        Artwork(state.artwork, Modifier.size(coverSize).align(Alignment.Center))
        AnimatedVisibility(!controls, Modifier.align(Alignment.TopCenter).padding(top = 24.dp), fadeIn(tween(250)), fadeOut(tween(250))) {
            Label(clock, 26, weight = FontWeight.Medium)
        }
        AnimatedVisibility(!controls, Modifier.align(Alignment.BottomCenter).padding(horizontal = 24.dp, vertical = 24.dp), fadeIn(tween(250)), fadeOut(tween(250))) {
            val line = remember(state.title, state.artist) {
                buildAnnotatedString {
                    withStyle(SpanStyle(color = Color.White)) { append(state.title) }
                    withStyle(SpanStyle(color = Color.White.copy(alpha = .6f))) { append(" · "); append(state.artist) }
                }
            }
            BasicText(line, Modifier.widthIn(max = 680.dp).clipToBounds().basicMarquee(iterations = Int.MAX_VALUE),
                style = TextStyle(fontSize = 22.sp, fontWeight = FontWeight.Medium), maxLines = 1)
        }
        AnimatedVisibility(controls, Modifier.fillMaxSize(), fadeIn(tween(250)), fadeOut(tween(250))) {
            Box(Modifier.fillMaxSize()) {
                Box(Modifier.fillMaxWidth().fillMaxHeight(.74f).align(Alignment.BottomCenter)
                    .background(Brush.verticalGradient(listOf(Color.Transparent, Color.Black.copy(alpha = .70f)))))
                Column(Modifier.align(Alignment.BottomCenter).padding(start = 24.dp, top = 24.dp, end = 24.dp).widthIn(max = 680.dp).fillMaxWidth()) {
                    Label(state.title, 28, Modifier.fillMaxWidth().clipToBounds().basicMarquee(iterations = Int.MAX_VALUE), FontWeight.Medium)
                    Label(state.artist, 19, Modifier.padding(top = 2.dp).fillMaxWidth().clipToBounds().basicMarquee(iterations = Int.MAX_VALUE), color = Color.White.copy(alpha = .6f))
                    Spacer(Modifier.height(18.dp))
                    val fraction = if (state.positionValid && state.duration > 0)
                        (state.position.toFloat() / state.duration).coerceIn(0f, 1f) else 0f
                    Canvas(Modifier.fillMaxWidth().height(5.dp).semantics {
                        contentDescription = "播放进度，只读"
                        progressBarRangeInfo = if (state.positionValid && state.duration > 0)
                            ProgressBarRangeInfo(fraction, 0f..1f) else ProgressBarRangeInfo.Indeterminate
                    }) {
                        drawRoundRect(Color.White.copy(alpha = .28f), cornerRadius = CornerRadius(size.height))
                        if (fraction > 0) drawRoundRect(Color.White, size = Size(size.width * fraction, size.height), cornerRadius = CornerRadius(size.height))
                    }
                    Row(Modifier.fillMaxWidth().padding(top = 6.dp), horizontalArrangement = Arrangement.SpaceBetween) {
                        Label(if (state.positionValid) formatTime(state.position) else "—:—", 13, color = Color.White.copy(alpha = .6f))
                        Label(if (state.positionValid && state.duration > 0) "−${formatTime((state.duration - state.position).coerceAtLeast(0))}" else "—:—", 13, color = Color.White.copy(alpha = .6f))
                    }
                    Row(Modifier.fillMaxWidth().padding(top = 4.dp), horizontalArrangement = Arrangement.SpaceEvenly, verticalAlignment = Alignment.CenterVertically) {
                        TransportButton("上一首", 0, state.previous) { command("previous") }
                        TransportButton(if (state.playing) "暂停" else "播放", if (state.playing) 2 else 1, state.transport) {
                            command(if (state.playing) "pause" else "play")
                        }
                        TransportButton("下一首", 3, state.next) { command("next") }
                    }
                    Box(Modifier.fillMaxWidth().height(18.dp), contentAlignment = Alignment.Center) {
                        Label(state.message, 11, color = Color.White.copy(alpha = .7f))
                    }
                }
            }
        }
    }
}

@Composable
private fun Artwork(bitmap: Bitmap?, modifier: Modifier) {
    Box(modifier.shadow(18.dp, RoundedCornerShape(20.dp)).clip(RoundedCornerShape(20.dp))
        .background(Color(0xFF78909C)), contentAlignment = Alignment.Center) {
        if (bitmap != null) Image(bitmap.asImageBitmap(), "专辑封面", Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
        else Canvas(Modifier.fillMaxSize(.40f).semantics { contentDescription = "暂无专辑封面" }) {
            val stroke = size.minDimension * .09f
            val x = size.width * .66f
            drawLine(Color.White.copy(alpha = .65f), Offset(x, size.height * .17f), Offset(x, size.height * .74f), stroke)
            drawLine(Color.White.copy(alpha = .65f), Offset(x - stroke / 2, size.height * .17f), Offset(size.width * .90f, size.height * .10f), stroke * 1.5f)
            drawOval(Color.White.copy(alpha = .65f), Offset(size.width * .28f, size.height * .63f), Size(size.width * .43f, size.height * .28f))
        }
    }
}

@Composable
private fun Label(text: String, size: Int, modifier: Modifier = Modifier, weight: FontWeight = FontWeight.Normal, color: Color = Color.White) {
    BasicText(text, modifier, style = TextStyle(color = color, fontSize = size.sp, fontWeight = weight,
        fontFeatureSettings = "tnum"), maxLines = 1)
}

@Composable
private fun TransportButton(description: String, kind: Int, enabled: Boolean, action: () -> Unit) {
    Box(Modifier.size(width = 88.dp, height = 64.dp).clip(RoundedCornerShape(16.dp))
        .clickable(enabled = enabled, role = Role.Button, onClick = action)
        .semantics { contentDescription = description }, contentAlignment = Alignment.Center) {
        Canvas(Modifier.size(if (kind == 1 || kind == 2) 44.dp else 40.dp)) {
            val color = Color.White.copy(alpha = if (enabled) 1f else .3f)
            fun triangle(a: Float, b: Float, c: Float) {
                drawPath(Path().apply {
                    moveTo(size.width * a, size.height * .18f)
                    lineTo(size.width * b, size.height * .5f)
                    lineTo(size.width * c, size.height * .82f)
                    close()
                }, color)
            }
            when (kind) {
                2 -> {
                    drawRoundRect(color, Offset(size.width * .23f, size.height * .12f), Size(size.width * .19f, size.height * .76f), CornerRadius(3.dp.toPx()))
                    drawRoundRect(color, Offset(size.width * .58f, size.height * .12f), Size(size.width * .19f, size.height * .76f), CornerRadius(3.dp.toPx()))
                }
                1 -> triangle(.24f, .84f, .24f)
                0 -> { triangle(.49f, .05f, .49f); triangle(.95f, .51f, .95f) }
                else -> { triangle(.05f, .49f, .05f); triangle(.51f, .95f, .51f) }
            }
        }
    }
}

private fun formatTime(milliseconds: Long): String {
    val seconds = milliseconds.coerceAtLeast(0) / 1_000
    return if (seconds >= 3_600) String.format(Locale.ROOT, "%d:%02d:%02d", seconds / 3_600, seconds / 60 % 60, seconds % 60)
    else String.format(Locale.ROOT, "%d:%02d", seconds / 60, seconds % 60)
}

// 在低分辨率封面上执行可分离高斯卷积，后台计算一次，不使用 API31 RenderEffect。
private fun gaussianBackdrop(art: Bitmap): Bitmap {
    val scale = 96f / maxOf(art.width, art.height)
    val width = (art.width * scale).roundToInt().coerceAtLeast(1)
    val height = (art.height * scale).roundToInt().coerceAtLeast(1)
    val small = art.scale(width, height, true)
    val source = IntArray(width * height)
    small.getPixels(source, 0, width, 0, 0, width, height)
    if (small !== art) small.recycle()
    val radius = 12
    val kernel = FloatArray(radius * 2 + 1) { i -> exp(-((i - radius) * (i - radius)) / 72f) }
    val sum = kernel.sum()
    for (i in kernel.indices) kernel[i] /= sum
    fun pass(input: IntArray, horizontal: Boolean): IntArray {
        return IntArray(input.size) { index ->
            val x = index % width
            val y = index / width
            var r = 0f; var g = 0f; var b = 0f
            for (k in -radius..radius) {
                val sx = if (horizontal) (x + k).coerceIn(0, width - 1) else x
                val sy = if (horizontal) y else (y + k).coerceIn(0, height - 1)
                val pixel = input[sy * width + sx]
                val weight = kernel[k + radius]
                r += (pixel shr 16 and 255) * weight
                g += (pixel shr 8 and 255) * weight
                b += (pixel and 255) * weight
            }
            android.graphics.Color.rgb(r.roundToInt().coerceIn(0, 255), g.roundToInt().coerceIn(0, 255), b.roundToInt().coerceIn(0, 255))
        }
    }
    return Bitmap.createBitmap(pass(pass(source, true), false), width, height, Bitmap.Config.ARGB_8888)
}

@Preview(name = "展示态 · 800×480", widthDp = 800, heightDp = 480, showBackground = true, backgroundColor = 0xFF526C79)
@Composable
private fun DisplayPreview() {
    PlayerLayout(PlayerSnapshot(title = "雨天", artist = "孙燕姿"), false, "9:03") {}
}

@Preview(name = "控制态 · 800×480", widthDp = 800, heightDp = 480, showBackground = true, backgroundColor = 0xFF526C79)
@Composable
private fun ControlsPreview() {
    PlayerLayout(PlayerSnapshot(title = "FEVER DREAM", artist = "Alex Warren", playing = true,
        position = 96_000, duration = 153_000, positionValid = true,
        previous = true, transport = true, next = true, message = ""), true, "9:03") {}
}

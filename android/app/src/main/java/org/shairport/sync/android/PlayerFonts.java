package org.shairport.sync.android;

import java.util.regex.Pattern;

final class PlayerFonts {
    // 显式把汉字交给系统无衬线字体，避免按语言回退时选到其他字体。
    static final Pattern HAN = Pattern.compile("\\p{sc=Han}+");

    private PlayerFonts() {}
}

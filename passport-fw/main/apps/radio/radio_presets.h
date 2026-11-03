#pragma once
#include "radio_player.h"

// Shared presets: one catalog failure must not leave the radio with one station.
inline constexpr RadioStation kRadioPresets[] = {
#ifdef PASSPORT_RADIO_HLS_TEST
    {"经济之声", "HLS测试 · TS/AAC", "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8", 0},
    {"华语金曲", "HLS测试 · AAC分片", "http://ls.qingting.fm/live/3412131.m3u8?bitrate=64", 0},
    {"凤凰资讯", "HLS测试 · TS/AAC", "http://playtv-live.ifeng.com/live/06OLEEWQKN4_audio.m3u8", 0},
    {"SomaFM", "美国 · MP3测试", "https://ice6.somafm.com/groovesalad-128-mp3", 0},
    {"MANGORADIO", "德国 · MP3测试", "https://mangoradio.stream.laut.fm/mangoradio", 0},
    {"中国之声", "MP3回归测试", "http://lhttp.qingting.fm/live/15318317/64k.mp3", 0},
#else
    {"中国之声", "推荐电台 · 新闻综合", "http://lhttp.qingting.fm/live/15318317/64k.mp3", 0},
    {"怀旧音乐", "推荐电台 · 经典老歌", "http://lhttp.qingting.fm/live/4804/64k.mp3", 0},
    {"清晨音乐", "推荐电台 · 轻松旋律", "http://lhttp.qingting.fm/live/4915/64k.mp3", 0},
    {"两广之声", "推荐电台 · 粤语音乐", "http://lhttp.qingting.fm/live/20500149/64k.mp3", 0},
    {"羊城交通", "推荐电台 · 城市资讯", "http://lhttp.qingting.fm/live/1262/64k.mp3", 0},
    {"第一财经", "推荐电台 · 财经资讯", "http://lhttp.qingting.fm/live/276/64k.mp3", 0},
#endif
};

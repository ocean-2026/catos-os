#!/usr/bin/env python3
"""
gen_pinyin.py -- build the pinyin -> hanzi candidate table for the IME.

Produces src/pinyin.inc, a compact read-only table of all 400+ Mandarin
syllables found in GB2312 with their homophones, ordered so that the
most useful characters come first.  The generated .inc is committed;
the kernel build has NO dependency on Python, pypinyin or jieba.

Candidate ordering (best available wins):
  1. jieba's word-frequency dictionary, if importable;
  2. a hand-curated PREFERRED list for the most common syllables;
  3. GB2312 level 1 (common) before level 2 (rare), then code order.

Build-time dependencies (optional, regeneration only):
    python -m pip install --target <dir> pypinyin jieba
    PYTHONPATH=<dir> python kernel_v25/tools/gen_pinyin.py

Licensing: the table is a factual pinyin->character mapping.  pypinyin
(MIT) and jieba (MIT) are used only as build-time tools; neither is
shipped in the kernel.  The hanzi bitmaps themselves come from GNU
Unifont (see tools/UNIFONT-LICENSE.txt).
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "pinyin.inc")

# Common syllables: characters a user is most likely to want, in order.
# Anything not listed falls through to the frequency/GB2312 ordering.
PREFERRED = {
    "a": "啊阿",
    "ai": "爱哎唉挨矮碍癌艾哀",
    "an": "安按案暗岸俺",
    "ba": "吧把爸八巴拔罢霸",
    "bai": "白百摆败拜",
    "ban": "班般搬板半办伴",
    "bang": "帮邦榜膀棒",
    "bao": "包保报抱饱宝暴薄",
    "bei": "被北备背倍杯悲贝",
    "ben": "本笨奔",
    "bi": "比笔必避壁毕鼻币彼",
    "bian": "边变便遍编辩",
    "biao": "表标彪",
    "bie": "别憋",
    "bin": "宾滨彬",
    "bing": "并病兵冰饼",
    "bo": "波播拨伯博薄勃",
    "bu": "不部布步补捕",
    "cai": "才菜采财材猜裁",
    "can": "参餐残惨灿",
    "cang": "藏苍舱",
    "cao": "草操曹槽",
    "ce": "测策侧册",
    "ceng": "层曾",
    "cha": "查茶察差插叉",
    "chai": "拆柴",
    "chan": "产颤缠掺",
    "chang": "长常场唱厂尝畅",
    "chao": "超朝潮吵抄",
    "che": "车彻撤扯",
    "chen": "陈晨沉衬称臣",
    "cheng": "成城程称乘承诚",
    "chi": "吃持迟池尺赤翅",
    "chong": "充冲重崇宠",
    "chou": "抽愁仇丑臭筹",
    "chu": "出初处除楚触储",
    "chuan": "穿传船串川",
    "chuang": "窗床创闯",
    "chui": "吹垂锤",
    "chun": "春纯唇",
    "ci": "次此词磁辞刺",
    "cong": "从丛聪",
    "cu": "粗促醋",
    "cuan": "窜",
    "cui": "催脆翠",
    "cun": "村存寸",
    "cuo": "错措挫",
    "da": "大达打答搭",
    "dai": "带代待戴袋呆贷",
    "dan": "但单淡担蛋旦",
    "dang": "当党挡档荡",
    "dao": "到道倒岛导刀盗",
    "de": "的得德",
    "deng": "等登灯邓瞪",
    "di": "地第低底敌递帝",
    "dian": "点电店典垫",
    "diao": "掉调钓雕",
    "ding": "定顶订丁钉",
    "dong": "动东懂冬洞董",
    "dou": "都斗豆抖兜",
    "du": "度读独毒渡肚",
    "duan": "段断短端",
    "dui": "对队堆兑",
    "dun": "顿盾蹲",
    "duo": "多朵躲夺舵",
    "e": "饿恶额俄鹅",
    "er": "而二儿耳尔",
    "fa": "发法罚乏伐",
    "fan": "反饭范犯翻烦凡返",
    "fang": "方放房防访仿纺",
    "fei": "飞非费肥废肺",
    "fen": "分份粉奋愤纷",
    "feng": "风封逢蜂锋丰疯奉",
    "fo": "佛",
    "fou": "否",
    "fu": "服复父付富负福附妇幅",
    "gai": "该改盖概",
    "gan": "干感赶敢甘肝",
    "gang": "刚钢岗港缸",
    "gao": "高告搞稿膏",
    "ge": "个各歌格哥隔革",
    "gei": "给",
    "gen": "跟根",
    "geng": "更耕",
    "gong": "工公共供功攻宫恭",
    "gou": "够狗构购勾",
    "gu": "故古顾固姑骨鼓",
    "gua": "挂刮瓜寡",
    "guai": "怪拐乖",
    "guan": "关观管惯馆冠官灌",
    "guang": "光广逛",
    "gui": "归规贵鬼柜桂跪",
    "gun": "滚棍",
    "guo": "过国果锅裹",
    "hai": "还海害孩嗨",
    "han": "汉喊寒汗含韩旱",
    "hang": "行航杭",
    "hao": "好号毫豪耗浩",
    "he": "和喝合河何盒贺荷",
    "hei": "黑嘿",
    "hen": "很狠恨痕",
    "heng": "横恒",
    "hong": "红洪宏轰虹",
    "hou": "后候厚猴喉",
    "hu": "胡湖呼虎护互忽户",
    "hua": "话花化画划华滑",
    "huai": "坏怀淮",
    "huan": "换还欢环缓患幻",
    "huang": "黄慌皇荒晃",
    "hui": "会回挥灰恢汇慧悔",
    "hun": "婚混昏魂",
    "huo": "火活或货获伙",
    "ji": "几机鸡记计及级即集极急寄季",
    "jia": "家加假价架甲佳夹",
    "jian": "见件间简建坚检健剑渐",
    "jiang": "将江讲奖降姜疆",
    "jiao": "叫教脚交角觉焦较骄",
    "jie": "接街节解界姐借结介届",
    "jin": "进近今金紧仅尽禁津",
    "jing": "经京静精景警竟境净",
    "jiu": "就九久酒旧救究",
    "ju": "句举据局居剧距具巨",
    "juan": "卷捐倦",
    "jue": "觉决绝角掘",
    "jun": "军君均俊",
    "ka": "卡咖",
    "kai": "开凯慨",
    "kan": "看刊砍堪",
    "kang": "康抗扛",
    "kao": "考靠烤",
    "ke": "可科课克客刻渴颗",
    "ken": "肯垦恳",
    "keng": "坑",
    "kong": "空控孔恐",
    "kou": "口扣寇",
    "ku": "苦哭库裤枯",
    "kua": "跨夸垮",
    "kuai": "快块筷",
    "kuan": "宽款",
    "kuang": "况狂矿框",
    "kui": "亏愧葵",
    "kun": "困昆",
    "kuo": "扩阔括",
    "la": "啦拉辣腊落",
    "lai": "来赖",
    "lan": "蓝烂懒拦栏览",
    "lang": "浪狼郎朗",
    "lao": "老劳牢捞",
    "le": "了乐勒",
    "lei": "类累泪雷垒",
    "leng": "冷愣",
    "li": "里理力立李例离礼历丽利",
    "lian": "连脸练联恋廉",
    "liang": "两量亮辆凉梁良",
    "liao": "了聊料疗辽",
    "lie": "列烈裂劣",
    "lin": "林临邻淋",
    "ling": "领令另零灵龄铃",
    "liu": "六流留刘柳",
    "long": "龙笼隆聋",
    "lou": "楼漏搂",
    "lu": "路录露陆炉鹿",
    "lv": "律旅绿滤吕",
    "luan": "乱",
    "lun": "论轮伦",
    "luo": "落罗络洛螺",
    "ma": "吗妈马码骂嘛麻",
    "mai": "买买卖麦脉",
    "man": "满慢漫瞒",
    "mang": "忙盲茫",
    "mao": "毛猫冒帽貌贸",
    "me": "么",
    "mei": "没每美妹梅媒",
    "men": "们门闷",
    "meng": "梦猛蒙盟",
    "mi": "米密迷秘蜜弥",
    "mian": "面免棉眠勉",
    "miao": "秒苗妙描庙",
    "mie": "灭",
    "min": "民敏",
    "ming": "明名命鸣",
    "mo": "摸莫末模默磨墨",
    "mou": "某谋",
    "mu": "母木目幕墓慕牧",
    "na": "那拿哪纳娜",
    "nai": "奶耐乃",
    "nan": "南难男",
    "nao": "脑闹恼",
    "ne": "呢",
    "nei": "内",
    "neng": "能",
    "ni": "你呢泥尼拟逆匿腻",
    "nian": "年念娘酿",
    "niao": "鸟尿",
    "nie": "捏",
    "nin": "您",
    "ning": "宁凝",
    "niu": "牛扭纽",
    "nong": "农弄浓",
    "nu": "努怒奴",
    "nv": "女",
    "nuan": "暖",
    "nuo": "诺挪",
    "ou": "欧偶呕",
    "pa": "怕爬帕",
    "pai": "排派拍牌",
    "pan": "判盘盼叛",
    "pang": "旁胖",
    "pao": "跑抛泡炮",
    "pei": "陪配培赔",
    "pen": "喷盆",
    "peng": "朋碰棚鹏捧",
    "pi": "皮批疲匹辟",
    "pian": "片便骗偏",
    "piao": "票飘漂",
    "pie": "撇",
    "pin": "品拼贫频",
    "ping": "平凭评瓶萍",
    "po": "破迫坡颇泼",
    "pu": "普铺扑朴谱",
    "qi": "起其气七期奇齐企器骑",
    "qia": "恰掐",
    "qian": "前钱千签浅欠迁潜",
    "qiang": "强枪墙抢腔",
    "qiao": "桥瞧巧敲悄乔",
    "qie": "且切窃",
    "qin": "亲勤秦琴侵",
    "qing": "请清情青轻晴倾庆",
    "qiu": "求球秋丘囚",
    "qu": "去区取曲趣渠驱",
    "quan": "全权劝圈泉拳",
    "que": "却确缺雀",
    "qun": "群",
    "ran": "然燃染",
    "rang": "让嚷",
    "rao": "绕扰饶",
    "re": "热惹",
    "ren": "人任认忍仁",
    "reng": "仍扔",
    "ri": "日",
    "rong": "容荣融绒",
    "rou": "肉柔",
    "ru": "如入乳儒",
    "ruan": "软",
    "rui": "瑞锐",
    "run": "润",
    "ruo": "若弱",
    "sa": "撒洒",
    "sai": "赛塞",
    "san": "三散伞",
    "sang": "桑丧嗓",
    "sao": "扫嫂骚",
    "se": "色涩",
    "sen": "森",
    "sha": "杀沙傻纱厦",
    "shai": "晒",
    "shan": "山善闪扇衫删",
    "shang": "上商伤尚赏",
    "shao": "少烧稍勺绍",
    "she": "设社射舌舍蛇摄",
    "shei": "谁",
    "shen": "身深什神甚审伸",
    "sheng": "生声省升胜圣剩",
    "shi": "是时事实使十市师式诗石识始示士视世失",
    "shou": "收手受首守兽瘦授",
    "shu": "书数树输叔熟属术述束",
    "shua": "刷耍",
    "shuai": "帅摔甩衰",
    "shuan": "拴",
    "shuang": "双爽霜",
    "shui": "水谁睡税",
    "shun": "顺瞬",
    "shuo": "说硕",
    "si": "四死思斯私司丝似",
    "song": "送松宋耸",
    "sou": "搜艘",
    "su": "速素诉苏宿塑俗",
    "suan": "算酸",
    "sui": "岁随虽碎隋",
    "sun": "孙损笋",
    "suo": "所锁索缩",
    "ta": "他她它踏塔塌",
    "tai": "太台抬态泰",
    "tan": "谈弹探摊坛贪坦",
    "tang": "糖汤堂躺趟唐",
    "tao": "套逃桃讨淘陶",
    "te": "特",
    "teng": "疼腾藤",
    "ti": "题提体替梯踢蹄",
    "tian": "天田添填甜",
    "tiao": "条跳挑调",
    "tie": "铁贴帖",
    "ting": "听停厅挺庭艇",
    "tong": "同通痛统童铜桶",
    "tou": "头投偷透",
    "tu": "图土突途徒吐兔",
    "tuan": "团推腿退",
    "tui": "推腿退褪",
    "tun": "吞屯",
    "tuo": "脱托拖妥拓",
    "wa": "挖哇娃袜瓦",
    "wai": "外歪",
    "wan": "完晚万玩碗弯湾",
    "wang": "王往望网忘亡旺",
    "wei": "为位未味喂胃威微围唯危伟卫",
    "wen": "问文闻稳温纹吻",
    "weng": "翁",
    "wo": "我握窝卧沃",
    "wu": "五无物务舞午屋误武雾悟",
    "xi": "西吸希习洗喜系细戏席息",
    "xia": "下夏吓虾霞峡",
    "xian": "先现线县显仙闲险限",
    "xiang": "想向象相香响像乡详",
    "xiao": "小笑校消晓效孝萧",
    "xie": "些写谢鞋斜协血携",
    "xin": "心新信欣辛薪",
    "xing": "行性星醒型姓幸形",
    "xiong": "兄雄胸熊凶",
    "xiu": "修休秀袖绣",
    "xu": "需须许续序虚徐蓄",
    "xuan": "选宣悬旋玄",
    "xue": "学雪血穴",
    "xun": "寻讯训迅巡循",
    "ya": "呀压牙亚鸭雅芽",
    "yan": "眼言演验严烟沿研盐延",
    "yang": "样阳养洋羊扬仰氧",
    "yao": "要药摇腰咬邀遥",
    "ye": "也夜业叶爷野页液",
    "yi": "一以已意义议衣依医移易亦异艺亿忆",
    "yin": "因音印银饮引隐",
    "ying": "应英影营硬迎赢映",
    "yo": "哟",
    "yong": "用永勇拥涌",
    "you": "有又右游友油优由尤犹",
    "yu": "于与语雨鱼遇预育欲玉愈",
    "yuan": "元原远愿院员圆园源",
    "yue": "月约越阅跃岳",
    "yun": "云运允晕韵孕",
    "za": "杂砸",
    "zai": "在再载灾宰",
    "zan": "咱赞暂",
    "zang": "脏藏葬",
    "zao": "早造遭糟澡燥",
    "ze": "责则择",
    "zei": "贼",
    "zen": "怎",
    "zeng": "增赠曾",
    "zha": "炸扎闸眨",
    "zhai": "摘窄宅债",
    "zhan": "站占战展沾粘",
    "zhang": "张章长涨帐掌丈",
    "zhao": "找照招朝兆赵",
    "zhe": "这着者折哲浙",
    "zhen": "真镇阵振珍诊",
    "zheng": "正整证争政挣征",
    "zhi": "知只之直支制值指纸至治志质职脂",
    "zhong": "中种重众终钟忠",
    "zhou": "周州洲舟粥皱",
    "zhu": "主住注助猪逐竹煮祝著",
    "zhua": "抓",
    "zhuan": "转专砖赚",
    "zhuang": "装状庄撞壮",
    "zhui": "追坠",
    "zhun": "准",
    "zhuo": "桌捉着卓酌",
    "zi": "子自字资紫姿仔",
    "zong": "总宗纵综",
    "zou": "走奏揍",
    "zu": "组足族租阻祖",
    "zuan": "钻",
    "zui": "最嘴醉罪",
    "zun": "尊遵",
    "zuo": "做作坐左座昨",
}

HEADER = """\
/* auto-generated by tools/gen_pinyin.py -- DO NOT EDIT.
 * Pinyin -> GB2312 homophone candidates for the built-in IME.
 * Each `hz` is a NUL-terminated UTF-8 string; every hanzi is exactly
 * 3 bytes (all BMP), so candidate i starts at hz + 3*i.
 * Sorted by pinyin for binary search.  Candidates are ordered by
 * frequency / usefulness so the first page is the one users want.
 * Regeneration needs pypinyin (MIT, build-time only); jieba is optional.
 */
typedef struct { const char py[8]; const char *hz; } py_entry_t;
static const py_entry_t py_tab[] = {
"""


def gb2312_chars():
    out = []
    for qu in range(0xB0, 0xF8):
        for wei in range(0xA1, 0xFF):
            try:
                out.append((qu, bytes([qu, wei]).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    return out


def utf8_escapes(s):
    return "".join("\\x%02X" % b for b in s.encode("utf-8"))


def main():
    try:
        from pypinyin import pinyin, Style
    except ImportError:
        sys.exit("ERROR: pypinyin is required to regenerate (pip install "
                 "pypinyin); the committed src/pinyin.inc needs nothing.")

    freq = None
    try:
        import jieba
        jieba.initialize()          # lazy-load dict.txt
        freq = jieba.dt.FREQ
        print("frequency source: jieba (%d entries)" % len(freq))
    except Exception as e:
        print("frequency source: PREFERRED list + GB2312 level order (%s)"
              % type(e).__name__)

    chars = gb2312_chars()
    groups = {}
    for qu, ch in chars:
        py = pinyin(ch, style=Style.NORMAL)[0][0]
        # pypinyin writes the umlaut as "v" in some versions, "nv"/"lv".
        py = py.replace("ü", "v")
        groups.setdefault(py, []).append((qu, ch))

    def rank(py, item):
        qu, ch = item
        pref = PREFERRED.get(py, "")
        if ch in pref:
            key = (0, pref.index(ch))
        elif freq is not None and freq.get(ch, 0) > 0:
            key = (1, -freq.get(ch, 0), qu)
        else:
            # level 1 (qu <= 0xD7) before level 2
            key = (2, 0 if qu <= 0xD7 else 1, qu)
        return key

    parts = [HEADER]
    for py in sorted(groups):
        ordered = sorted(groups[py], key=lambda it: rank(py, it))
        # make the hand-curated characters win over the generic rank
        pref = PREFERRED.get(py, "")
        ordered = sorted(ordered, key=lambda it:
                         (0, pref.index(it[1])) if it[1] in pref
                         else (1, rank(py, it)))
        hz = "".join(ch for _, ch in ordered)
        parts.append('    {"%s", "%s"},   /* %d */\n'
                     % (py, utf8_escapes(hz), len(ordered)))
    parts.append("};\n")
    parts.append("#define PY_TAB_COUNT ((int)(sizeof(py_tab)/sizeof(py_tab[0])))\n")
    with open(OUT, "w", encoding="ascii", newline="\n") as fh:
        fh.write("".join(parts))
    print("wrote %s: %d syllables, %d hanzi, %d bytes"
          % (OUT, len(groups), len(chars), os.path.getsize(OUT)))


if __name__ == "__main__":
    main()

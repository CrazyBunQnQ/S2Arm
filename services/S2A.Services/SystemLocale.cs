using Ryujinx.HLE.HOS.SystemState;
using System;
using System.Runtime.InteropServices;

namespace S2A.Services
{
    // 系统语言缺省跟设备（维护者 10-20：「都要装上以后默认是中文版哈（系统本身也是中文版的）」）：
    //   没给 S2A_SYSTEM_LANGUAGE 时按 iPhone / Mac 的系统首选语言（CFLocaleCopyPreferredLanguages，BCP 47 标签如 zh-Hans-CN）映射 Switch SystemLanguage；
    //   测试资产 / 对真值局显式给的 S2A_SYSTEM_LANGUAGE 照旧优先。游戏不支持该语言时按 TitleFallback 的四步选。
    //   服务层是 InvariantGlobalization，CultureInfo 拿不到系统语言，所以直接调 CoreFoundation（iOS / macOS 同一个 C 接口）
    public static class SystemLocale
    {
        // BCP 47 标签 → Switch 系统语言；认不出返回 null（换首选列表里的下一个）
        public static SystemLanguage? Map(string tag)
        {
            if (string.IsNullOrEmpty(tag))
            {
                return null;
            }

            string[] p = tag.Replace('_', '-').ToLowerInvariant().Split('-');
            string lang = p[0];
            bool Has(string s) => Array.IndexOf(p, s, 1) >= 0;

            switch (lang)
            {
                case "zh":
                    // 有文字子标签按文字定；只有地区时：台 / 港 / 澳 = 繁体，其余简体
                    if (Has("hant")) return SystemLanguage.TraditionalChinese;
                    if (Has("hans")) return SystemLanguage.SimplifiedChinese;
                    return Has("tw") || Has("hk") || Has("mo") ? SystemLanguage.TraditionalChinese : SystemLanguage.SimplifiedChinese;
                case "ja": return SystemLanguage.Japanese;
                case "ko": return SystemLanguage.Korean;
                case "en": return Has("gb") || Has("au") || Has("nz") || Has("ie") || Has("in") || Has("za") ? SystemLanguage.BritishEnglish : SystemLanguage.AmericanEnglish;
                case "fr": return Has("ca") ? SystemLanguage.CanadianFrench : SystemLanguage.French;
                case "es": return p.Length == 1 || Has("es") ? SystemLanguage.Spanish : SystemLanguage.LatinAmericanSpanish;
                case "pt": return Has("br") ? SystemLanguage.BrazilianPortuguese : SystemLanguage.Portuguese;
                case "de": return SystemLanguage.German;
                case "it": return SystemLanguage.Italian;
                case "nl": return SystemLanguage.Dutch;
                case "ru": return SystemLanguage.Russian;
                case "pl": return SystemLanguage.Polish;
                case "th": return SystemLanguage.Thai;
                default: return null;
            }
        }

        // 首选语言列表里第一个认得的；都不认得（或取不到）回美式英语。来源写进 why 供日志
        public static SystemLanguage FromPreferred(string[] tags, out string why)
        {
            foreach (string t in tags)
            {
                SystemLanguage? l = Map(t);

                if (l.HasValue)
                {
                    why = $"系统首选语言 {t}";
                    return l.Value;
                }
            }

            why = tags.Length == 0 ? "取不到系统首选语言" : $"系统首选语言 {string.Join(",", tags)} 都没有对应的 Switch 语言";
            return SystemLanguage.AmericanEnglish;
        }

        // 游戏不支持系统语言时依次试的语言（交 Ryujinx GetDesiredLanguage，S2AHooks.TitleLanguageFallback）。只看宿主主语言 primary
        //   （Host.Language：玩家显式指定的语言优先，否则宿主首选列表第一项；维护者 11-10「导出终端根据咱们宿主来识别语言种类」），不逐个走首选列表：
        //   ① 主语言 → ② 它的同族变体（简中 ↔ 繁中、美式 ↔ 英式英语、法语 ↔ 加拿大法语、西班牙语 ↔ 拉美西语、葡语 ↔ 巴西葡语）→ ③ 美式 / 英式英语 → ④ 简中 / 繁中 → ⑤ 游戏支持表第一项（上游）。
        //   ④ 留住 11-03 的修法（亚洲版只收韩 / 繁 / 简，上游取第一个 = 韩语）：玩家绝大多数是中文用户，宿主英语也多半是中文玩家设的英文系统，简 / 繁比韩语更可能看得懂。
        //   同族紧跟是真机 ns 服务语言优先表的口径（逆向资料：简中表 = 简中、繁中、美式英语、英式英语、日语…）。
        //   #445：游戏只声明美 / 日 / 韩 / 繁（0x3005），旧拼法「首选列表逐个 → 简中 → 繁中」让首选列表后面的日语 / 英语先于繁中命中，中文玩家看到日文 / 英文
        public static TitleLanguage[] TitleFallback(SystemLanguage primary)
        {
            var r = new System.Collections.Generic.List<TitleLanguage>();
            void Add(TitleLanguage t) { if (!r.Contains(t)) r.Add(t); }
            if (Enum.TryParse(primary.ToString(), out TitleLanguage p))
            {
                Add(p);
                if (Sibling(p) is TitleLanguage s) Add(s);
            }
            Add(TitleLanguage.AmericanEnglish); Add(TitleLanguage.BritishEnglish);
            Add(TitleLanguage.SimplifiedChinese); Add(TitleLanguage.TraditionalChinese);
            return r.ToArray();
        }

        public static TitleLanguage? Sibling(TitleLanguage t) => t switch
        {
            TitleLanguage.SimplifiedChinese => TitleLanguage.TraditionalChinese,
            TitleLanguage.TraditionalChinese => TitleLanguage.SimplifiedChinese,
            TitleLanguage.AmericanEnglish => TitleLanguage.BritishEnglish,
            TitleLanguage.BritishEnglish => TitleLanguage.AmericanEnglish,
            TitleLanguage.French => TitleLanguage.CanadianFrench,
            TitleLanguage.CanadianFrench => TitleLanguage.French,
            TitleLanguage.Spanish => TitleLanguage.LatinAmericanSpanish,
            TitleLanguage.LatinAmericanSpanish => TitleLanguage.Spanish,
            TitleLanguage.Portuguese => TitleLanguage.BrazilianPortuguese,
            TitleLanguage.BrazilianPortuguese => TitleLanguage.Portuguese,
            _ => null,
        };

        public static string[] Preferred()
        {
            IntPtr arr = CFLocaleCopyPreferredLanguages();

            if (arr == IntPtr.Zero)
            {
                return Array.Empty<string>();
            }

            try
            {
                long n = CFArrayGetCount(arr);
                string[] r = new string[n];
                byte[] buf = new byte[128];

                for (long i = 0; i < n; i++)
                {
                    IntPtr s = CFArrayGetValueAtIndex(arr, i);
                    r[i] = s != IntPtr.Zero && CFStringGetCString(s, buf, buf.Length, 0x08000100) ? System.Text.Encoding.UTF8.GetString(buf, 0, Array.IndexOf(buf, (byte)0)) : "";
                }

                return r;
            }
            finally
            {
                CFRelease(arr);
            }
        }

        private const string CF = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";

        [DllImport(CF)] private static extern IntPtr CFLocaleCopyPreferredLanguages();
        [DllImport(CF)] private static extern long CFArrayGetCount(IntPtr array);
        [DllImport(CF)] private static extern IntPtr CFArrayGetValueAtIndex(IntPtr array, long index);
        [DllImport(CF)] [return: MarshalAs(UnmanagedType.I1)] private static extern bool CFStringGetCString(IntPtr s, byte[] buffer, long size, uint encoding);
        [DllImport(CF)] private static extern void CFRelease(IntPtr cf);
    }
}

// 金手指文本解析（面板列条目、导入前校验都用它；只依赖 Foundation，Mac 单测 runtime/tests/t_cheats_parse.m 直接编）。
//   规则与服务层 services/S2A.Services/Cheats.cs Parse 一致：[名字] 普通条目，{名字} 主代码，其余非空行是指令；括号不闭合整份回 nil。
//   cur / master / lines 必须是 __block：flush 是 block，不加则捕获的是创建那一刻的值（lines 恒为 0），任何文件都解析成 0 条、导入一律报「不是 Atmosphere 格式」（#458）
#import <Foundation/Foundation.h>

NSArray<NSDictionary *> *s2a_cheats_parse(NSString *text);   // 对外声明在 ios/vpad_cheats.h（那边带 UIKit，这里不引）
NSArray<NSDictionary *> *s2a_cheats_parse(NSString *text) {
    NSMutableArray *out = [NSMutableArray array]; __block NSString *cur = nil; __block BOOL master = NO; __block NSInteger lines = 0;
    void (^flush)(void) = ^{ if (lines > 0) [out addObject:@{@"name": cur ?: @"未命名", @"master": @(master)}]; };
    for (NSString *raw in [text componentsSeparatedByString:@"\n"]) {
        NSString *l = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (!l.length) continue;
        unichar c = [l characterAtIndex:0];
        if (c == '[' || c == '{') {
            unichar close = c == '[' ? ']' : '}';
            if (l.length < 2 || [l characterAtIndex:l.length - 1] != close) return nil;
            flush(); lines = 0; master = c == '{';
            cur = [[l substringWithRange:NSMakeRange(1, l.length - 2)] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            if (!cur.length) cur = master ? @"主代码" : @"未命名";
        } else lines++;
    }
    flush();
    return out;
}

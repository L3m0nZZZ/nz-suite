#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""NZ广播(nzg) 一键构建 + 版本归档
用法: python build_nzg.py [版本标签, 如 v0.1.1_95台_20260912]
产物: D:/ningz/nzg_versions/<版本标签>/ 下 APK + deploy_config.json + channels.json + src/
"""
import os, sys, json, shutil, subprocess, zipfile, datetime

PROJ   = 'D:/ningz/nzg'
OUTBASE = 'D:/ningz/nzg_versions'
JDK    = 'D:/ningz/.buildtools/x/jdk-17.0.20.1+1'
SDK    = 'D:/ningz/.buildtools/sdk'
GRADLE = 'D:/ningz/.buildtools/x/gradle-8.7/bin/gradle.bat'
SRC    = 'C:/Users/zlst/WorkBuddy/2026-09-11-09-44-00'


def count_from_assets():
    p = os.path.join(PROJ, 'app/src/main/assets/config_default.json')
    return len(json.load(open(p, encoding='utf-8')).get('channels', []))


def build():
    env = dict(os.environ)
    env['JAVA_HOME'] = JDK
    env['ANDROID_HOME'] = SDK
    env['ANDROID_SDK_ROOT'] = SDK
    print('>>> gradle assembleDebug ...')
    r = subprocess.run([GRADLE, 'assembleDebug', '--no-daemon'],
                       cwd=PROJ, env=env, shell=False)
    if r.returncode != 0:
        print('构建失败'); sys.exit(1)


def extract_config(apk, out):
    with zipfile.ZipFile(apk) as z:
        n = next((x for x in z.namelist() if x.endswith('assets/config_default.json')), None)
        if not n: return False
        open(out, 'wb').write(z.read(n))
    return True


def main():
    n = count_from_assets()
    tag = sys.argv[1] if len(sys.argv) > 1 else \
        'v%s_%d台_%s' % (json.load(open(os.path.join(PROJ, 'app/build.gradle.kts'),
                                       encoding='utf-8')) and '0.1.0', n,
                         datetime.datetime.now().strftime('%Y%m%d'))
    build()
    apk = os.path.join(PROJ, 'app/build/outputs/apk/debug/app-debug.apk')
    d = os.path.join(OUTBASE, tag)
    os.makedirs(os.path.join(d, 'src'), exist_ok=True)
    apk_name = '守播_nzg_%03d_%s.apk' % (n, datetime.datetime.now().strftime('%Y%m%d'))
    shutil.copy2(apk, os.path.join(d, apk_name))
    ok = extract_config(apk, os.path.join(d, 'config_default.json'))
    for f in ['deploy_config.json', 'channels.json']:
        src = os.path.join(PROJ, f)
        if os.path.exists(src): shutil.copy2(src, os.path.join(d, f))
    for f in ['gen_nzg.py', 'nzg_channels.json']:
        src = os.path.join(SRC, f)
        if os.path.exists(src): shutil.copy2(src, os.path.join(d, 'src', f))
    print('归档: %s' % d)
    print('  APK=%s  内置config提取=%s  台数=%d' % (apk_name, ok, n))


if __name__ == '__main__':
    main()

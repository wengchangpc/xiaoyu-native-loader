# Xiaoyu loader probe
# Runs INSIDE the injected game process via PyRun_SimpleString.
#
# ASCII ONLY on purpose: py2.7 hands raw bytes to the parser with no coding
# cookie, so a single non-ASCII byte here is a SyntaxError before line 1.
# Keep it 2.7 / 3.x compatible: no f-strings, no nonlocal, print() only.

import sys
import os
import io
import traceback

LOG = r'C:\Xiaoyu\probe.log'
MAX_LIST = 60


def _ensure_dir():
    try:
        d = os.path.dirname(LOG)
        if d and not os.path.isdir(d):
            os.makedirs(d)
    except Exception:
        pass


def _to_text(line):
    """Coerce anything to unicode/str; py2 byte strings are decoded as UTF-8."""
    try:
        if isinstance(line, type(u'')):
            return line
    except Exception:
        pass
    if sys.version_info[0] < 3:
        if isinstance(line, str):
            try:
                return line.decode('utf-8', 'replace')
            except Exception:
                return line
        try:
            return unicode(line)  # noqa: F821  (py2 only)
        except Exception:
            try:
                return str(line)
            except Exception:
                return u'<unprintable>'
    return str(line)


def w(line):
    _ensure_dir()
    text = _to_text(line)
    try:
        f = io.open(LOG, 'a', encoding='utf-8', errors='replace')
        try:
            f.write(text)
            f.write(u'\n')
        finally:
            f.close()
    except Exception:
        try:
            f = open(LOG, 'ab')
            try:
                f.write((text + u'\n').encode('utf-8', 'replace'))
            finally:
                f.close()
        except Exception:
            pass


def section(title):
    w(u'')
    w(u'==== ' + title + u' ====')


def safe(label, fn):
    try:
        return fn()
    except Exception:
        w(u'  ! ' + label + u' -> EXC ' + repr(sys.exc_info()[1]))
        return None


def names_of(obj):
    out = []
    for n in dir(obj):
        if n.startswith('__'):
            continue
        out.append(n)
    out.sort()
    return out


def dump_names(label, obj):
    ns = safe(label + ' dir()', lambda: names_of(obj))
    if ns is None:
        return
    w(u'  ' + label + u' total ' + str(len(ns)) + u': ' + u', '.join(ns[:MAX_LIST]))
    if len(ns) > MAX_LIST:
        w(u'  ' + label + u' rest ' + str(len(ns) - MAX_LIST) + u' omitted')


def probe():
    section(u'env')
    w(u'python: ' + sys.version.replace(u'\n', u' '))
    w(u'executable: ' + str(sys.executable))
    w(u'prefix: ' + str(sys.prefix))
    w(u'maxunicode: ' + str(sys.maxunicode))
    w(u'defaultencoding: ' + str(sys.getdefaultencoding()))
    w(u'cwd: ' + str(os.getcwd()))
    w(u'sys.path: ' + repr(sys.path[:8]))

    section(u'sys.modules / ModSDK related')
    keys = []
    for k in sys.modules.keys():
        low = k.lower()
        if ('netease' in low) or ('pynet' in low) or low == u'mod' \
                or low.startswith(u'mod.') or ('modsdk' in low):
            keys.append(k)
    keys.sort()
    w(u'matched ' + str(len(keys)) + u' modules:')
    for k in keys[:MAX_LIST]:
        w(u'  ' + k)

    section(u'_pynetmodule')
    try:
        import _pynetmodule as pn
        w(u'import OK')
        dump_names(u'_pynetmodule', pn)
        for attr in (u'pyaddon', u'send2server', u'sendToServer', u'call'):
            w(u'  has ' + attr + u': ' + str(hasattr(pn, attr)))
    except Exception:
        w(u'import FAIL: ' + repr(sys.exc_info()[1]))

    section(u'mod.client.extraClientApi')
    clientApi = None
    try:
        import mod.client.extraClientApi as clientApi
        w(u'import OK')
        dump_names(u'extraClientApi', clientApi)
        for attr in (u'RegisterSystem', u'GetSystem', u'RegisterUI', u'GetUI',
                     u'GetEngineCompFactory', u'GetLocalPlayerId', u'GetLevelId',
                     u'GetMinecraftEnum', u'GetTouchPos', u'GetInputVector',
                     u'NotifyToServer', u'GetScreenNodeCls', u'GetUIRenderEntityCls'):
            w(u'  has ' + attr + u': ' + str(hasattr(clientApi, attr)))
    except Exception:
        w(u'import FAIL: ' + repr(sys.exc_info()[1]))

    section(u'mod.common.mod')
    try:
        import mod.common.mod as mmod
        w(u'import OK')
        dump_names(u'mod.common.mod', mmod)
    except Exception:
        w(u'import FAIL: ' + repr(sys.exc_info()[1]))

    section(u'engine factory / system probes')
    if clientApi is not None:
        def _factory():
            f = clientApi.GetEngineCompFactory()
            w(u'  GetEngineCompFactory -> ' + repr(type(f)))
            dump_names(u'factory', f)
            return f
        safe(u'GetEngineCompFactory', _factory)

        def _local_player():
            pid = clientApi.GetLocalPlayerId()
            w(u'  GetLocalPlayerId -> ' + repr(pid))
            return pid
        safe(u'GetLocalPlayerId', _local_player)

        def _level():
            w(u'  GetLevelId -> ' + repr(clientApi.GetLevelId()))
        safe(u'GetLevelId', _level)

        def _systems():
            for ns, name in ((u'Xiaoyu', u'XiaoyuClientSystem'),
                             (u'pyaddon', u'PyAddonClientSystem')):
                try:
                    s = clientApi.GetSystem(ns, name)
                    w(u'  GetSystem(' + ns + u',' + name + u') -> ' + repr(s))
                except Exception:
                    w(u'  ! GetSystem(' + ns + u',' + name + u') EXC '
                      + repr(sys.exc_info()[1]))
        safe(u'GetSystem', _systems)

        def _ui():
            for ns, name in ((u'Xiaoyu', u'clickGui'), (u'Xiaoyu', u'arrayListGui')):
                try:
                    u = clientApi.GetUI(ns, name)
                    w(u'  GetUI(' + ns + u',' + name + u') -> ' + repr(u))
                except Exception:
                    w(u'  ! GetUI(' + ns + u',' + name + u') EXC '
                      + repr(sys.exc_info()[1]))
        safe(u'GetUI', _ui)

    section(u'how to read this')
    w(u'[R1 OK]      _pynetmodule and extraClientApi both import OK, and')
    w(u'             GetEngineCompFactory returns an object -> the DLL can stay')
    w(u'             a pure loader and the Python client can be reused as-is.')
    w(u'[R1 delayed] imports fail here but succeed after entering a world ->')
    w(u'             inject only after the world is loaded.')
    w(u'[R2 needed]  imports still fail inside a world -> ModSDK layer is out of')
    w(u'             reach, GUI must be redrawn natively (D3D11 hook).')


try:
    w(u'')
    w(u'################ probe start ################')
    probe()
    w(u'################ probe end ################')
except Exception:
    w(u'probe crashed:')
    try:
        w(traceback.format_exc())
    except Exception:
        pass

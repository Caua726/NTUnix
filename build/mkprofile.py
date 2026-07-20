#!/usr/bin/env python3
"""Gera a lista de remocao de um perfil, para o wimlib aplicar DENTRO do wim.

    wimlib-imagex dir IMG.wim N --detailed | mkprofile.py <perfil> > cmds.txt
    wimlib-imagex update IMG.wim N < cmds.txt

Por que stripar no wim, no host, e nao depois de aplicar no disco: o conteudo do
Windows e' hardlinkado entre WinSxS e as copias vivas, entao apagar por caminho
no disco quase nao libera nada — a copia sobrevive do outro lado. Dentro do wim
o armazenamento e' por hash: sumindo todas as referencias, o dado sai de fato.
E de quebra a midia encolhe junto (a do perfil debug fica ~700MB, nao 7,6G).

Os tres perfis e os tamanhos abaixo foram MEDIDOS sobre um Windows 11 Pro
(13,00 GB de conteudo unico); ver docs/perfis.md.
"""
import re
import sys

# ---------------------------------------------------------------------------
# comum aos tres: o user space da Microsoft que a NTUnix existe para substituir
# ---------------------------------------------------------------------------
COMUM = (
    '/program files/windowsapps/', '/windows/systemapps/',            # UWP/AppX
    '/program files (x86)/microsoft/edge', '/windows/system32/microsoft-edge',
    '/program files/windows defender', '/programdata/microsoft/windows defender/',
    '/program files/windows defender advanced threat protection/',
    '/windows/system32/recovery/',                                     # WinRE
    '/windows/systemresources/', '/windows/servicing/',
    '/windows/ime/', '/windows/system32/ime/',
    '/windows/system32/config/components',                             # registro do CBS
    '/windows/system32/catroot2/', '/windows/system32/securebootupdates/',
    '/windows/system32/windowspowershell/', '/windows/system32/wbem/repository/',
    '/windows/uus/', '/windows/speech', '/windows/system32/speech',
    '/windows/system32/onedrivesetup.exe', '/windows/syswow64/onedrivesetup.exe',
    '/windows/winsxs/backup/',
)

# WinSxS: poda SELETIVA. Apagar o component store inteiro quebra a resolucao de
# manifesto — apps Win32 pedem Microsoft.Windows.Common-Controls 6.0 e nao abrem
# sem ela (erro 14001). Manter os assemblies certos custa ~82MB de 3,04GB.
SXS_ASSEMBLY = ('common-controls', 'vclibs', 'mfc', 'gdiplus', 'comctl',
                'msvcrt', 'vc80', 'vc90', 'ucrt', 'mfcore')
SXS_DIR = ('manifests', 'filemaps', 'catalogs')   # catalogs valida assinatura de driver

# System32: categorias por prefixo de nome de arquivo
CATS = {
    'winrt':     ('windows.', 'winrt'),
    'ie':        ('mshtml', 'edgehtml', 'ieframe', 'iertutil', 'jscript', 'chakra',
                  'vbscript', 'urlmon', 'inetcpl', 'msfeeds'),
    'print':     ('print', 'spool', 'xps', 'prn', 'win32spl', 'localspl', 'mxdc'),
    'rdp':       ('rdp', 'mstsc', 'termsrv', 'wtsapi', 'sessenv', 'tsgqec'),
    'bitlocker': ('fve', 'bde', 'tpm', 'ngc'),
    'search':    ('search', 'tquery', 'mssrch', 'mssph'),
    'rasvpn':    ('rasman', 'rastls', 'rasppp', 'sstp', 'agilevpn', 'tapi', 'telephon'),
    'xbox':      ('xbox', 'gaming', 'xinput', 'xaudio'),
    'sensor':    ('sensor', 'bth', 'bluetooth', 'nfc', 'winbio', 'wpd'),
    'ml':        ('onnx', 'winml', 'directml'),
    'backup':    ('vss', 'swprv', 'wbadmin', 'wbengine', 'sdclt'),
    'hyperv':    ('vmbus', 'vmicvss', 'hcs', 'vhdmp', 'storvsp', 'vmcompute'),
    'wmp':       ('wmp', 'wmv', 'wma', 'msmpeg', 'dolby', 'msaudio'),
    'ocr':       ('ocr', 'ink', 'tabtip', 'wisp', 'narrator', 'magnify', 'osk.'),
    'wifi':      ('wlan', 'wifi', 'dot3', 'wwan', 'mbae'),
    'refs':      ('refs', 'mispace', 'spaceport', 'dedup'),
    'debugger':  ('dbgeng', 'symsrv', 'werfault', 'wermgr', 'werui', 'faultrep'),
    'telemetria':('diagtrack', 'utcapi', 'aeinv', 'appraiser', 'compattel', 'dmwappush'),
    'reset':     ('reset', 'cloudrecovery', 'srtasks', 'recdisc', 'systemreset'),
    'microcode': ('mcupdate',),
    'deploy':    ('dism', 'wimgapi', 'imagex', 'setupcl', 'sysprep', 'unattend'),
    'iscsi':     ('iscsi', 'mpio', 'msdsm', 'fcinfo'),
}
# avulsos que nenhuma categoria pega e que nada nosso usa
AVULSOS = ('wincsflags.exe', 'srh.dll', 'bootux.dll', 'ntkrla57.exe')

# drivers mantidos quando o perfil restringe (nomes de .sys e de pacote INF)
DRV_VM = ('storahci', 'viostor', 'netkvm', 'e1000', 'intelide', 'usbhid', 'kbdhid',
          'mouhid', 'hidusb', 'usbxhci', 'acpi', 'disk', 'volume', 'partmgr',
          'volmgr', 'fltmgr', 'ntfs', 'fastfat', 'tcpip', 'ndis', 'afd', 'netbt',
          'http', 'vga', 'basicdisplay', 'basicrender', 'monitor', 'msisadrv',
          'pci', 'pdc', 'cng', 'ksecdd', 'mup', 'volsnap', 'crashdmp')
FONTES = ('segoeui', 'seguisb', 'seguisym', 'consola', 'arial', 'tahoma', 'cour',
          'times', 'marlett', 'symbol')
# o WinPE nao traz estas, mas Chrome e Firefox exigem — nunca remover
BROWSER = {'comctl32.dll', 'dxgi.dll', 'd3d11.dll', 'd3d10warp.dll', 'mfplat.dll',
           'mfcore.dll', 'mf.dll', 'mfreadwrite.dll', 'dcomp.dll', 'd2d1.dll',
           'dwrite.dll', 'd3dcompiler_47.dll', 'windowscodecs.dll',
           'windowscodecsext.dll', 'uiautomationcore.dll', 'msvcp140.dll',
           'vcruntime140.dll', 'vcruntime140_1.dll', 'taskmgr.exe'}

PERFIS = {
    # ~4,9 GB — compatibilidade maxima: .NET, 32 bits e drivers completos ficam
    'normal': dict(net=True, wow=True, drv='todos', fonts='todas', mui=True,
                   cats=('winrt', 'ie', 'ml', 'xbox', 'ocr', 'search', 'rdp',
                         'bitlocker', 'hyperv', 'sensor', 'backup', 'telemetria',
                         'reset', 'refs', 'iscsi')),
    # ~2,9 GB — sem .NET nem 32 bits; drivers genericos, ainda instalavel em metal
    'leve':   dict(net=False, wow=False, drv='todos', fonts='todas', mui=False,
                   cats=tuple(k for k in CATS if k not in ('winrt',)) + ('winrt',)),
    # ~690 MB — SO VM. Whitelist derivada do WinPE: um conjunto que comprovadamente
    # roda NT + Win32 (o proprio ambiente live), mais o enxerto para browser.
    'debug':  dict(net=False, wow=False, drv='vm', fonts='latinas', mui=False,
                   cats=tuple(CATS), whitelist=True),
}


def carrega_whitelist(caminho):
    """Nomes de arquivo do System32 do WinPE = conjunto minimo comprovado."""
    nomes = set()
    with open(caminho) as fh:
        for linha in fh:
            for p in linha.rstrip('\n').split('\t')[-1].split('|'):
                q = p.lower()
                if q.startswith('/windows/system32/') and '/' not in q[18:]:
                    nomes.add(q[18:])
    return nomes


def mantem_debug(q, f, wl):
    """Perfil debug: KEEP-ONLY. O que nao estiver aqui sai.

    Semantica invertida de proposito. Nos outros perfis listamos o que remover e
    o resto fica; aqui o alvo (~690MB) so' e' alcancavel listando o que fica —
    uma lista de remocao equivalente teria milhares de linhas e envelheceria mal.
    """
    if q.startswith('/windows/system32/'):
        resto = q[len('/windows/system32/'):]
        if resto.startswith('drivers/'):
            return f.endswith('.sys') and any(d in f for d in DRV_VM)
        if resto.startswith('driverstore/filerepository/'):
            return any(d in q.split('/filerepository/')[1][:24] for d in DRV_VM)
        if resto.startswith('config/'):     # hives; COMPONENTS e logs saem no COMUM
            return not (f.startswith('components') or f.endswith(('.log1', '.log2', '.blf')))
        if resto.startswith('en-us/'):
            return True
        if '/' in resto:                    # wbem/, migration/, catroot/, ...
            return False
        if f in BROWSER:
            return True
        return resto in wl                  # conjunto minimo do WinPE
    if q.startswith('/windows/winsxs/'):
        partes = q.split('/')
        return len(partes) < 4 or partes[3] in SXS_DIR or \
            any(k in partes[3] for k in SXS_ASSEMBLY)
    if q.startswith('/windows/fonts/'):
        return f.startswith(FONTES)
    if q.startswith('/windows/boot/'):
        return '/fonts/' not in q and ('/en-us/' in q or q.count('/') <= 4)
    if q.startswith('/windows/inf/'):
        return f.endswith('.inf') and any(d in f for d in DRV_VM)
    return q.startswith('/windows/globalization/sorting/')


def remove(p, cfg, wl):
    """True se o caminho deve sair da imagem neste perfil."""
    q = p.lower()
    f = q.rsplit('/', 1)[-1]

    if any(q.startswith(r) for r in COMUM):
        return True

    if cfg.get('whitelist'):
        if f in AVULSOS or any(f.startswith(CATS[c]) for c in cfg['cats']):
            return True
        return not mantem_debug(q, f, wl)

    if q.startswith('/windows/winsxs/'):
        partes = q.split('/')
        if len(partes) < 4:
            return False
        d = partes[3]
        return not (d in SXS_DIR or any(k in d for k in SXS_ASSEMBLY))

    if not cfg['net'] and q.startswith(('/windows/assembly/', '/windows/microsoft.net/')):
        return True
    if not cfg['wow'] and q.startswith(('/windows/syswow64/', '/program files (x86)/')):
        return True
    if not cfg['mui'] and f.endswith('.mui'):
        return True

    if q.startswith('/windows/fonts/'):
        return cfg['fonts'] != 'todas' and not f.startswith(FONTES)

    if '/driverstore/filerepository/' in q or q.startswith('/windows/system32/drivers/'):
        if cfg['drv'] == 'todos':
            return False
        alvo = q.split('/filerepository/')[1][:24] if '/filerepository/' in q else f
        return not any(d in alvo for d in DRV_VM)

    if q.startswith('/windows/system32/'):
        if f in BROWSER:
            return False
        resto = q[len('/windows/system32/'):]
        if '/' in resto:                       # subdiretorios tratados acima
            return False
        if cfg.get('whitelist'):
            if resto not in wl:                # fora do conjunto do WinPE
                return True
        if f in AVULSOS:
            return True
        for nome in cfg['cats']:
            if f.startswith(CATS[nome]):
                return True
    return False


def main():
    perfil = sys.argv[1] if len(sys.argv) > 1 else 'normal'
    if perfil not in PERFIS:
        sys.exit(f'perfil desconhecido: {perfil} (use: {", ".join(PERFIS)})')
    cfg = PERFIS[perfil]
    wl = set()
    if cfg.get('whitelist'):
        pe = sys.argv[2] if len(sys.argv) > 2 else None
        if not pe:
            sys.exit('perfil debug exige o listing do WinPE como 2o argumento')
        wl = carrega_whitelist(pe)
        if len(wl) < 500:
            sys.exit(f'whitelist do WinPE suspeita: so {len(wl)} nomes')

    caminho = None
    vistos, apagados, bytes_out = set(), 0, 0
    for linha in sys.stdin:
        if linha.startswith('Full Path'):
            caminho = linha.split('= ', 1)[1].strip().strip('"')
            if caminho and caminho not in vistos and remove(caminho, cfg, wl):
                vistos.add(caminho)
                apagados += 1
                print("delete --force --recursive '%s'" % caminho.replace('/', '\\'))
    print(f'# perfil {perfil}: {apagados} caminhos removidos', file=sys.stderr)


if __name__ == '__main__':
    main()

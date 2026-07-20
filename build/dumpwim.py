#!/usr/bin/env python3
"""Reduz o listing --detailed do wimlib a um TSV compacto: tamanho + caminhos.

Usado para (a) montar a whitelist do perfil debug a partir do WinPE e (b) medir
tamanho real por conteudo unico — o listing cru tem ~20 linhas por arquivo.
"""
import sys
cur = {}
caminho = pend = None
for linha in sys.stdin:
    if linha.startswith('Full Path'):
        caminho = linha.split('= ', 1)[1].strip().strip('"'); pend = None
    elif linha.startswith('Hash '):
        h = linha.split('= ', 1)[1].strip(); pend = h if h.strip('0x0') else None
    elif linha.startswith('Uncompressed size') and pend and caminho:
        n = int(linha.split('= ', 1)[1].split()[0])
        if n:
            cur.setdefault(pend, [n, []])[1].append(caminho)
        pend = None
for _, (n, ps) in cur.items():
    print(f"{n}\t{'|'.join(ps)}")

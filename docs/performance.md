# Desempenho

Números medidos com um board sintético de **2000 cards em 12 listas** (4 listas "quentes" de ~12
cards e 8 listas "frias" de ~244), 8 etiquetas e 6 membros. Máquina de desenvolvimento: Ubuntu,
GNOME/Wayland, GTK 4.22, libadwaita 1.9.

Metas do design: card em cache abre em < 100 ms; board de ~2000 cards renderiza em < 500 ms; nada
de rede ou disco na thread de interface.

## Como reproduzir

```
# core (sem interface), contra um servidor HTTP local de verdade:
cmake --build build --target core_bench && build/tests/core_bench

# interface GTK contra o Trello de mentira:
python3 tools/mock_trello.py --port 8099 --cards 2000 &
CARDINAL_TRACE=1 build/ui-gtk/cardinal_gtk --api-base http://127.0.0.1:8099/1 \
    --api-key K --token T0123456789abcdefghij --data-dir /tmp/cardinal-demo
# CARDINAL_TRACE_FRAMES=1 mostra cada frame; --open-card c1 abre um card; --snapshot f.png tira foto.
```

## Core (`tests/bench/core_bench.cpp`)

| Cenário | Resultado |
|---|---|
| Início a frio (cache vazio): primeira coluna com cards pronta para a UI | ~20 ms |
| Início a frio: as 12 listas / 2000 cards carregadas | ~175 ms |
| Requisições HTTP no início a frio | 15 (1 boards, 2 do board, 12 listas) |
| Início a quente (cache em disco, sem rede): `openBoard` → primeira coluna com cards | mediana 22 ms, p95 23 ms |
| `openCard` → painel com título, descrição, etiquetas e comentários (do cache) | mediana 0,24 ms, máx 0,4 ms |
| Requisições enviadas com a rede fora do ar | 0 |
| Com rede: `openCard` → descrição, comentários e extras todos frescos | mediana ~10 ms (loopback) |

Os tempos de rede acima são de loopback; com a internet somam-se os RTTs, mas a tela já está
pronta com o cache antes de qualquer resposta. O card mostra o que está no cache em < 1 ms de
trabalho e só depois chega o dado novo.

## Interface GTK (traces `CARDINAL_TRACE`)

| Momento | Tempo desde o início do processo |
|---|---|
| Janela apresentada | ~160 ms |
| Board com cards desenhado (cache quente) | ~370 ms (inclui ~100 ms de inicialização do GTK) |
| Abrir um card (clique → estado pronto) | 18 ms |
| Abrir um card (clique → primeiro frame com o card) | ~31 ms |
| Frames depois de tudo carregado | 16–17 ms (60 Hz) |

Histórico do que custou caro e o que resolveu (para não repetir):

1. `GtkListView` materializa **todas** as linhas do modelo que recebe (~0,33 ms/linha), mesmo fora
   da tela: oito colunas de 244 linhas paravam a interface por ~650 ms.
2. O modelo de cada coluna só é ligado ao `GtkListView` quando a coluna está perto da área
   visível. Fora da tela o `GListStore` é preenchido sem custo.
3. Cada `GtkListView` recebe só uma janela da coluna (40 linhas, +60 quando o scroll chega perto
   do fim), em vez das 244.
4. Antes da correção: ~950 ms até o board aparecer. Depois: ~370 ms.

## O que não foi medido

* Contra o Trello real (latência de rede, limites de requisição na prática). Sem credenciais, tudo
  foi medido contra `tools/mock_trello.py` e contra o servidor HTTP local dos testes.
* Memória (RSS) e consumo de CPU em repouso com polling a cada 20 s.
* Arrastar e soltar e rolagem com ponteiro real: o código existe, mas não foi exercitado por
  entrada real (a verificação foi por testes do core e por acionamento programático dos handlers).

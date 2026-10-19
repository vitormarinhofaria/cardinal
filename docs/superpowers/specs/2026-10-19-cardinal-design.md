# Cardinal: cliente Trello nativo e rápido (design)

## Intenção
O Trello web é lento em tarefas simples, como abrir um card, num board grande (~1600-2000 cards não arquivados, ~4 pessoas usando ao mesmo tempo). Cardinal é um cliente desktop nativo que mostra board e card imediatamente a partir de um cache local, atualiza em segundo plano e reduz requisições a dados pouco relevantes.

**Sucesso:** rápido, ágil, sem travamentos. Se funcionar assim, não há motivo para abandonar o app (palavras do usuário).

## Objetivos
- Abrir um card quase instantaneamente (cache local) com descrição, etiquetas e comentários.
- Listas pouco usadas ("frias") com carregamento de baixa prioridade.
- Layout o mais próximo possível do Trello atual.
- Interface nativa e desacoplada do núcleo, para permitir KDE, Windows e Mac no futuro.

## Não-objetivos da v1 (palpite do assistente, não confirmado)
Editar checklists e anexos (apenas leitura na v1), datas, power-ups, automações, criar boards, administração do workspace.

## Decisões do usuário
- Núcleo em **C++20**.
- Primeira interface: **GTK4 + libadwaita** (GNOME).
- Card aberto como **painel lateral à direita** (o board continua visível), não como diálogo central.
- Tempo real por **polling incremental** na v1, com a fonte de eventos atrás de uma interface.

- **Vários boards** com seletor de board; cache e polling por board.
- Checklists, anexos e demais dados não editáveis na v1 aparecem **somente leitura**, carregados em segundo plano.

## Funcionalidades da v1 (pedidas pelo usuário)
Ver boards, listas e cards; mover cards entre listas; criar cards; editar título e descrição; adicionar etiquetas e membros; comentar.

## Escolhas do assistente (a revisar)
- Autenticação: API key e token pessoais de cada usuário, guardados no chaveiro do sistema (libsecret); primeira execução pede as credenciais.
- O Trello é a fonte de verdade; o cache é uma cópia. Na primeira execução baixa-se a lista de boards, as listas e os cards das listas quentes; comentários e detalhes vêm sob demanda.
- Metas: card em cache abre em < 100 ms; board de ~2000 cards renderiza em < 500 ms; sem travamentos perceptíveis na UI (trabalho de rede e disco fora da thread da interface).
- Fila de escritas persistida em SQLite (sobrevive a reinício e a períodos offline); conflitos detectados pela data da última atividade do card, com aviso ao usuário.
- Listas quentes = abertas recentemente (janela a ajustar).
- Plataforma: Ubuntu/GNOME atuais, libadwaita recente; sem Flatpak na v1.

## Prioridade de dados (desempenho)
- Card aberto: descrição, etiquetas e comentários primeiro; checklists, anexos e membros depois, em segundo plano.
- Listas quentes (abertas recentemente) carregam os cards completos; listas frias mostram a contagem e carregam ao expandir ou em segundo plano com baixa prioridade (critério de quente/frio é proposta do assistente).
- Pedir à API só os campos usados na tela (`fields=`).
- Polling: consulta de ações novas do board a cada 15-30 s. Limites da API (~300 req/10 s por chave) vêm da memória e precisam ser conferidos.

## Arquitetura (proposta do assistente)
- `core/`: biblioteca C++20 sem dependência de UI. Client da API Trello (assíncrono, limite de taxa, filas por prioridade), cache SQLite, fila de escritas otimistas com reversão em caso de falha, interface `EventSource` (polling na v1), view-models observáveis.
- `ui-gtk/`: camada fina sobre os view-models.
- Futuro: `ui-qt/`, `ui-win/`, `ui-mac/` sobre o mesmo `core/`.
- Escritas otimistas: a mudança aparece na hora, a requisição segue em segundo plano; se falhar, reverte e avisa.

## Deixado ao builder
Escolha de bibliotecas (HTTP, JSON, SQLite), estrutura do CMake, fluxo de autenticação com API key + token do Trello, esquema do cache, estratégia de testes, empacotamento.

## Verificação
Medir tempo de abertura de card e de board (~2000 cards) com cache quente e frio; testar o `core` com API simulada; teste manual contra um board real.

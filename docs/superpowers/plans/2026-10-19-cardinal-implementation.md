# Cardinal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cliente Trello nativo (C++20 + GTK4/libadwaita) que abre board e card na hora a partir de cache local.

**Architecture:** `core/` é uma biblioteca estática sem UI (API Trello, cache SQLite, sync priorizado, fila de escritas, polling, view-models). `ui-gtk/` é uma camada fina sobre os view-models, comunicando-se por `UiDispatcher` para que outras UIs (Qt, Windows, Mac) reusem o core. Trabalho de rede e disco roda fora da thread de UI.

**Tech Stack:** C++20, CMake, libcurl, SQLite3, nlohmann/json, libsecret, GTK4, libadwaita, GoogleTest (pacotes apt do Ubuntu; conferir disponibilidade na Task 1).

**Spec:** `docs/superpowers/specs/2026-10-19-cardinal-design.md`

## Global Constraints
- Núcleo em C++20; primeira UI em GTK4 + libadwaita; `core/` não inclui nenhum header de GTK.
- Card aberto como **painel lateral direito**; vários boards com seletor.
- Polling incremental a cada 15-30 s atrás da interface `EventSource`.
- Pedir à API só os campos usados (`fields=`).
- Card em cache abre em < 100 ms; board de ~2000 cards renderiza em < 500 ms; nenhum I/O de rede ou disco na thread de UI.
- Fila de escritas persistida em SQLite; conflito detectado por `dateLastActivity`, com aviso.
- Token pessoal obtido pelo fluxo de autorização do Trello via navegador e servidor loopback (sem backend), guardado no chaveiro via libsecret; a API key é pública e embutida no build.
- Checklists e anexos somente leitura na v1.
- Limites da API (~300 req/10 s por chave, ~100/10 s por token) vêm da memória: conferir na documentação do Trello na Task 4.

## Review Focus
- Token inválido/expirado (401): UI pede credenciais de novo sem perder o cache.
- Rate limit (429): requisições esperam e retomam, sem falhar para o usuário.
- Card movido, arquivado ou apagado por outra pessoa enquanto o painel está aberto: painel avisa e atualiza.
- App aberto sem rede com escritas pendentes: mostra o cache e a fila é enviada ao reconectar.
- Lista fria com ~800 cards: expandir não trava a UI.
- Descrição/comentário muito longos e texto Unicode/emoji: render e edição íntegros.
- Board vazio ou lista sem cards: telas válidas.

---

### Task 1: Scaffold e build
**Files:** Create `CMakeLists.txt`, `core/CMakeLists.txt`, `ui-gtk/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/smoke_test.cpp`, `.clang-format`
**Interfaces:** Produces: alvos CMake `cardinal_core` (static), `cardinal_gtk` (executável), `core_tests`.
- [ ] Step 1: `apt` mostra os dev-packages (libcurl4-openssl-dev, libsqlite3-dev, nlohmann-json3-dev, libsecret-1-dev, libgtk-4-dev, libadwaita-1-dev, libgtest-dev); anotar versões do GTK/libadwaita no README.
- [ ] Step 2: Teste `Smoke.LinksCore` chama `cardinal::version()` e espera `"0.1.0"`; rodar e ver falhar (link).
- [ ] Step 3: Implementar `std::string_view cardinal::version()` em `core/include/cardinal/version.h`; configurar C++20, `-Wall -Wextra`.
- [ ] Step 4: `cmake -B build && cmake --build build && ctest --test-dir build` passa.
- [ ] Step 5: Commit `build: scaffold core, ui-gtk and tests`.

### Task 2: Modelo de domínio e parsing JSON
**Files:** Create `core/include/cardinal/model.h`, `core/src/model_json.cpp`; Test `tests/model_json_test.cpp` com fixtures em `tests/fixtures/*.json`
**Interfaces:** Produces: structs `Board{id,name,closed}`, `List{id,boardId,name,pos,cardCount}`, `Label{id,name,color}`, `Member{id,fullName,initials}`, `Comment{id,authorId,text,date}`, `Card{id,listId,name,desc,pos,labelIds,memberIds,commentCount,dateLastActivity}`; `Card parseCard(const nlohmann::json&)`, idem para os demais.
- [ ] Step 1: Testes `ParseCard.ReadsCoreFields`, `ParseCard.MissingOptionalFieldsAreEmpty`, `ParseCard.Utf8Emoji` (nome `"Café ☕ 🚀"` preservado byte a byte).
- [ ] Step 2: Rodar e ver falhar; implementar parsers tolerantes a campos ausentes; ver passar; commit.

### Task 3: Cache SQLite
**Files:** Create `core/include/cardinal/store.h`, `core/src/store.cpp`; Test `tests/store_test.cpp`
**Interfaces:** Consumes: Task 2 models. Produces: `class Store { static Store open(path); void upsertBoards(span<Board>); void upsertLists(...); void upsertCards(...); std::vector<List> lists(boardId); std::vector<Card> cards(listId); std::optional<Card> card(id); void setSyncCursor(boardId, std::string iso); std::optional<std::string> syncCursor(boardId); void touchList(listId, time_t); }` e tabela `pending_ops` (Task 8). Esquema versionado por `PRAGMA user_version`; WAL ativado.
- [ ] Step 1: Testes `Store.RoundTripsCard`, `Store.UpsertUpdatesExisting`, `Store.ReopenKeepsData` (arquivo temporário), `Store.EmptyListReturnsEmpty`.
- [ ] Step 2: Implementar com statements preparados e transações em lote; commit.

### Task 4: HTTP, agendador por prioridade e rate limit
**Files:** Create `core/include/cardinal/http.h`, `core/src/http_curl.cpp`, `core/src/scheduler.cpp`; Test `tests/scheduler_test.cpp`
**Interfaces:** Produces: `enum class Priority{Interactive, Visible, Background}`; `struct HttpResponse{int status; std::string body;}`; `class HttpClient { virtual std::future<HttpResponse> request(Method, std::string url, std::string body)=0; }`; `class Scheduler { Scheduler(HttpClient&, RateLimit, Clock&); std::future<HttpResponse> submit(Priority, Method, std::string url, std::string body=""); }`.
- [ ] Step 1: Conferir na doc do Trello os limites vigentes e usá-los como padrão de `RateLimit`.
- [ ] Step 2: Testes com `FakeClock`/`FakeHttp`: `Scheduler.InteractiveJumpsQueue`, `Scheduler.NeverExceedsWindow`, `Scheduler.RetriesAfter429WithBackoff`, `Scheduler.Http401IsSurfacedNotRetried`.
- [ ] Step 3: Implementar com thread worker única + curl multi (keep-alive/HTTP2); commit.

### Task 5: TrelloApi
**Files:** Create `core/include/cardinal/trello_api.h`, `core/src/trello_api.cpp`; Test `tests/trello_api_test.cpp`
**Interfaces:** Consumes: Scheduler, models. Produces: `class TrelloApi { std::future<std::vector<Board>> boards(); std::future<BoardSnapshot> board(boardId, Priority); /*listas+labels+members+cards mínimos*/ std::future<Card> cardCore(cardId); /*desc+labels+comments*/ std::future<CardExtras> cardExtras(cardId); /*checklists, anexos, membros*/ std::future<std::vector<Action>> actionsSince(boardId, iso); /*escritas:*/ moveCard, createCard, updateCard(name/desc), addLabel, addMember, addComment }`.
- [ ] Step 1: Testes com `FakeHttp` conferem URL, método e `fields=` mínimos (ex.: `board` não pede `desc` nem comentários), autenticação por query `key`/`token` e que `cardCore` usa `Priority::Interactive`.
- [ ] Step 2: Implementar; commit.

### Task 6: Sync e carregamento por camadas
**Files:** Create `core/include/cardinal/sync.h`, `core/src/sync.cpp`; Test `tests/sync_test.cpp`
**Interfaces:** Produces: `class SyncEngine { void start(); void openBoard(id); void openCard(id); void expandList(id); }`; callbacks via `ChangeSink::onChanged(ChangeSet)`.
- [ ] Step 1: Testes: `Sync.FirstRunLoadsBoardsListsAndHotCardsOnly`, `Sync.ColdListLoadsOnExpand`, `Sync.OpenCardServesCacheThenRefreshes`, `Sync.OpenCardLoadsExtrasAfterCore`, `Sync.ColdListsPrefetchedAtBackgroundPriority`.
- [ ] Step 2: Implementar (quente = `touchList` recente, janela configurável); commit.

### Task 7: EventSource e polling
**Files:** Create `core/include/cardinal/event_source.h`, `core/src/polling_event_source.cpp`; Test `tests/polling_test.cpp`
**Interfaces:** Produces: `class EventSource { virtual void watch(boardId, std::function<void(std::vector<Action>)>)=0; virtual void unwatch(boardId)=0; }`; `PollingEventSource(TrelloApi&, Store&, Duration interval)`.
- [ ] Step 1: Testes: `Polling.UsesStoredCursor`, `Polling.AdvancesCursorOnlyAfterApply`, `Polling.NoActionsMeansNoChange`, `Polling.StopsWhenBoardUnwatched`.
- [ ] Step 2: Implementar; sync aplica as ações ao cache; commit.

### Task 8: Fila de escritas otimistas
**Files:** Create `core/include/cardinal/write_queue.h`, `core/src/write_queue.cpp`; Test `tests/write_queue_test.cpp`
**Interfaces:** Consumes: Store, TrelloApi. Produces: `class WriteQueue { OpId enqueue(Op); }` com `Op = MoveCard | CreateCard | EditCard | AddLabel | AddMember | AddComment`; aplica a mudança no cache imediatamente e persiste em `pending_ops`.
- [ ] Step 1: Testes: `Queue.AppliesOptimisticallyThenConfirms`, `Queue.FailureRevertsAndNotifies`, `Queue.SurvivesRestart`, `Queue.ConflictWarnsWhenLastActivityChanged`, `Queue.OfflineKeepsOpsUntilReconnect`, `Queue.PreservesOrderPerCard`.
- [ ] Step 2: Implementar; commit.

### Task 9: Credenciais e login pelo navegador (sem backend)
**Files:** Create `core/include/cardinal/credentials.h`, `core/src/credentials_libsecret.cpp`, `core/include/cardinal/auth_flow.h`, `core/src/auth_flow.cpp`; Test `tests/credentials_test.cpp`, `tests/auth_flow_test.cpp`
**Interfaces:** Produces: `class CredentialStore { virtual std::optional<Credentials> load()=0; virtual void save(Credentials)=0; virtual void clear()=0; }`; `class AuthFlow { AuthFlow(std::string apiKey, BrowserOpener&); std::future<std::string> run(); }` que sobe um servidor em `127.0.0.1:<porta livre>`, abre `https://trello.com/1/authorize?key=<apiKey>&response_type=token&scope=read,write&expiration=never&callback_method=fragment&return_url=http://127.0.0.1:<porta>/callback`, serve em `/callback` uma página mínima com script que lê o fragment e faz POST de `token` para `/token`, devolve o token e encerra o servidor. A API key pública do app é constante de build, registrada no Trello com a origem local liberada; nenhum secret é embutido.
- [ ] Step 1: Testes com fake: `Credentials.RoundTrip`, `Credentials.InvalidTokenClearsAndRequestsLogin`, `AuthFlow.BuildsAuthorizeUrlWithLoopbackReturn`, `AuthFlow.ReturnsTokenPostedByCallbackPage`, `AuthFlow.IgnoresRequestsWithoutSessionNonce`, `AuthFlow.TimesOutAndClosesServer`.
- [ ] Step 2: Implementar libsecret e o AuthFlow (nonce de sessão na URL de retorno, servidor aceita só loopback e uma tentativa); commit.
- Login da UI (Task 11) vira o botão "Entrar com Trello", com colagem manual de token como alternativa.

### Task 10: View-models e UiDispatcher
**Files:** Create `core/include/cardinal/viewmodel.h`, `core/src/viewmodel.cpp`; Test `tests/viewmodel_test.cpp`
**Interfaces:** Produces: `class UiDispatcher { virtual void post(std::function<void()>)=0; }`; `BoardPickerVM`, `BoardVM{lists(), cards(listId), moveCard(...), createCard(...), expandList(...)}`, `CardPanelVM{state(), setTitle, setDescription, addLabel, addMember, addComment}`, todos com `Observable<T>`/assinaturas chamadas só via `UiDispatcher`.
- [ ] Step 1: Testes com dispatcher falso: `VM.NotifiesOnUiDispatcherOnly`, `VM.CardPanelShowsCacheImmediately`, `VM.PanelWarnsWhenCardRemovedRemotely`, `VM.EmptyBoardIsValid`.
- [ ] Step 2: Implementar; commit.

### Task 11: GTK shell, login e seletor de board
**Files:** Create `ui-gtk/src/main.cpp`, `ui-gtk/src/app_window.cpp`, `ui-gtk/src/login_page.cpp`, `ui-gtk/src/board_picker.cpp`; `ui-gtk/src/gtk_dispatcher.cpp` (usa `g_idle_add`)
- [ ] Step 1: App `AdwApplication`, primeira execução mostra "Entrar com Trello" (usa `AuthFlow`) e salva o token no `CredentialStore`.
- [ ] Step 2: Seletor de boards usa `BoardPickerVM`. Verificar rodando o app contra um board real; commit.

### Task 12: Board view
**Files:** Create `ui-gtk/src/board_view.cpp`, `ui-gtk/src/list_column.cpp`, `ui-gtk/src/card_row.cpp`
- [ ] Step 1: Colunas com `GtkListView` virtualizado, cards com etiquetas coloridas, contagem de comentários e membros; lista fria colapsada com "Expandir"; drag-and-drop de card entre listas chama `BoardVM::moveCard`.
- [ ] Step 2: Verificar com board de ~2000 cards: renderização < 500 ms, rolagem fluida (medir com `GTK_DEBUG=interactive`/logs de tempo); commit.

### Task 13: Painel lateral do card
**Files:** Create `ui-gtk/src/card_panel.cpp`
- [ ] Step 1: Painel à direita com título e descrição editáveis, etiquetas, membros, comentários (campo de novo comentário) e seções checklists/anexos somente leitura carregando em segundo plano.
- [ ] Step 2: Verificar: abrir card em cache < 100 ms; editar, comentar, adicionar etiqueta/membro refletem no Trello; commit.

### Task 14: Medição e polimento
**Files:** Create `tests/bench/open_card_bench.cpp`, `docs/performance.md`
- [ ] Step 1: Benchmarks de abertura de card (cache quente/frio) e de board (~2000 cards sintéticos); registrar números.
- [ ] Step 2: Tratar 401/429/offline na UI (barra de aviso, reautenticação); commit.

## Self-review
- Cobertura do spec: boards múltiplos (T10-11), prioridade de dados (T5-6), polling (T7), escritas otimistas/persistência/conflito (T8), credenciais (T9), painel lateral (T13), metas de desempenho (T12-14), somente leitura de checklists/anexos (T13).
- Fora desta rodada: UIs Qt/Windows/Mac.

## Verificação ponta a ponta
`cmake --build build && ctest --test-dir build` (todos os testes do core); executar `cardinal_gtk` contra um board real; conferir metas de desempenho da Task 14.

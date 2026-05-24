# Plano de atualização da tese WCAN

Campanha final: `transmitter/testing/results/final/` (684 testes, `repeats: 2`, perfil `full`).  
Diagrama mestre: `thesis/images/code_flow_tcc.drawio`.

Ordem de execução: **0 → 1 → 2 → 3 → 4 → 5 → 6** (cada bloco = um chat novo, copiar o prompt inteiro).

---

## Análise rápida do seu diagrama (`code_flow_tcc.drawio`)

### O que está bem coberto
- `send_data` → ring cheio, primeira amostra, timer `linger`, lote cheio, `finish_batch`, `push`
- `dispatch_batch` (aparentemente duas variantes visuais: preenchimento pontilhado vs hachura)
- `retry_task`: fila vazia, notify, envio à `radio_transmit_queue`, ACK, ≤4 tentativas, `on_radio_send`
- `send_processing_task`: serialize, MAC destino, `esp_now_send`, fila de resultado
- `esp_now_send_cb` → `on_radio_send`
- `esp_now_recv_cb` → ring RX cheio, enqueue
- `recv_processing_task` → `CONTROL_ID?` → `on_control_packet` / `on_data_packet`
- ACK no broadcast (`pending ack`, `sequence_id`)
- Multicast: `send subscription packet`, `update subscription table`

### Lacunas recomendadas (adicionar ou explicar na legenda do capítulo)
| Lacuna | Onde no código | Sugestão no diagrama ou no texto |
|--------|----------------|----------------------------------|
| Ramo **`linger_ms = 0`** (fecha  lote na 1ª amostra) | `TransceiverBase::send_data` | Losango “linger > 0?” já existe parcialmente; explicitar ramo “não → finish_batch imediato” |
| **`assign_new_sequence_id`** em `finish_batch` | `TransceiverBase::finish_batch` | Caixa em `finish_batch` |
| **`should_accept`** / filtro no ISR | `esp_now_recv_cb` | Antes do enqueue RX |
| **`Deduplicator`** | `recv_processing_task` | Entre decode e CONTROL? |
| **`management_task`** (multicast) | `wcan_multicast/Transceiver.cpp` | Caixa lateral ou subfluxo multicast |
| **`prepare_send_mac`** (FF:FF vs multicast HW) | `dispatch_batch` / send path | Nota na variante multicast |
| Multicast **sem `retry_task`** (envio direto à fila) | `Transceiver::dispatch_batch` multicast | Deixar explícito no ramo hachurado de `dispatch_batch` |
| Ring **CONTROL_ID** para ACK | `wcan_broadcast` init | Mini-caixa em `on_data_packet` / ACK |
| Legenda **dots vs hatch** | Estilos no draw.io | Legenda: ex. pontilhado = broadcast, hachura = multicast |
| Camada **`TransceiverBase`** + build-time | Arquitetura | Figura 5.1 separada (camadas), não só fluxograma |
| **`Stats`** / logs de teste | `Stats.cpp`, `WcanTest` | Opcional: caixa “WCAN_BATCH / R(RANGE)” no fim RX |

Não é obrigatório colocar tudo no mesmo canvas — o fluxograma gigante pode ser **fatias** exportadas (ver Passo 1).

---

## Passo 0 — Preparação (você, antes dos chats)

**Ações suas:**
1. Em `code_flow_tcc.drawio`, adicionar **legenda** (dots/hatch, ISR vs task se usar cores).
2. Corrigir lacunas críticas acima (pelo menos: `linger=0`, dedup, `should_accept`, legenda multicast vs broadcast).
3. **Não refazer o diagrama inteiro**. Usar as duas figuras já exportadas para o capítulo de Desenvolvimento:

| Arquivo | O que mostra | Uso no LaTeX |
|---------|--------------|--------------|
| `thesis/images/tx_path.svg` | Caminho de transmissão: `send_data`, ring TX, `linger`, `finish_batch`, `dispatch_batch`, fila de rádio, `send_processing_task`, `esp_now_send` e retorno por `on_radio_send` | § Núcleo / batching / transmissão |
| `thesis/images/rx_path.drawio.svg` | Caminho de recepção: `esp_now_recv_cb`, filtro/aceite, ring RX, `recv_processing_task`, deduplicação e separação entre pacotes de controle e dados | § Recepção / tratamento de pacotes |

Legenda visual das duas figuras:
- **Fundo pontilhado**: funções ou caminhos específicos da implementação broadcast.
- **Linhas diagonais**: funções ou caminhos específicos da implementação multicast.
- **Sem preenchimento especial**: núcleo comum, principalmente `TransceiverBase` e pipeline compartilhado.

Partes que devem ficar separadas no texto:
- **TX/batching** deve ser explicado a partir de `tx_path.svg`: `linger`, lote, `finish_batch`, `dispatch_batch`, fila de rádio e envio ESP-NOW.
- **RX/decodificação** deve ser explicado a partir de `rx_path.drawio.svg`: callback ESP-NOW, filtro/aceite, ring RX, deduplicação e roteamento entre controle e dados.
- **Variantes broadcast/multicast** devem ser explicadas dentro dessas duas figuras, usando a legenda de pontilhado e hachura, sem criar uma terceira figura.

4. Opcional: arquivar commit/hash do firmware testado num comentário no `run_plan.json` ou no apêndice.

---

## Passo 1 — Desenvolvimento

**Arquivo alvo:** `thesis/tex/capitulos/desenvolvimento.tex`  
**Referências obrigatórias:**  
`transmitter/components/wcan/inc/TransceiverBase.hpp`, `TransceiverBase.cpp`, `Packet.hpp`, `RingBuffer.hpp`, `wcan_broadcast/Transceiver.cpp`, `wcan_multicast/Transceiver.cpp`, `wcan_sensor/RampCanSensor.cpp`, `wcan_test/WcanTest.cpp`, `main/main.cpp`, `transmitter/DESIGN.md`, `thesis/images/code_flow_tcc.drawio` + PNG/SVG exportados no Passo 0.

**Prompt (copiar para chat novo):**

```
Atualize o capítulo de Desenvolvimento da monografia em português (ABNT, tom técnico).

Arquivo: thesis/tex/capitulos/desenvolvimento.tex
Diagrama mestre: thesis/images/code_flow_tcc.drawio (não reescreva o draw.io; usar só como referência visual secundária)
Figuras obrigatórias já exportadas pelo autor:
- thesis/images/tx_path.svg
- thesis/images/rx_path.drawio.svg
Código: transmitter/components/wcan/, wcan_broadcast/, wcan_multicast/, wcan_sensor/, wcan_test/, main/main.cpp
Design: transmitter/DESIGN.md

REGRAS:
- Remover texto obsoleto: data_packet_t, send_queue/recv_queue, 60 amostras fixas, tasks bloqueando em linger, wcan_recv_callback, CAN_PROC_*.
- Descrever arquitetura atual: TransceiverBase, RingBuffer TX/RX, esp_timer(linger), radio_transmit_queue, send/recv tasks, retry_task (só broadcast), Stats.
- Packet: CAN ID + sequence_id + data_count; MAX_DATA_POINTS conforme ESP-NOW da build.
- Duas variantes em tempo de compilação; semântica primeiro ACK (broadcast) vs MAC ack (multicast).
- Usar exatamente duas figuras principais:
  1. `tx_path.svg`: caminho de transmissão, batching, `linger`, `finish_batch`, `dispatch_batch`, fila de rádio, `send_processing_task`, `esp_now_send` e `on_radio_send`.
  2. `rx_path.drawio.svg`: caminho de recepção, `esp_now_recv_cb`, filtro/aceite, ring RX, `recv_processing_task`, deduplicação e separação `CONTROL_ID`/dados.
- Explicar a legenda visual das figuras: fundo pontilhado = broadcast; linhas diagonais = multicast; sem padrão especial = núcleo comum.
- Inserir as duas figuras com \includegraphics, \label{fig:wcan-tx-path} e \label{fig:wcan-rx-path}, e \caption em português.
- Usar as figuras como eixo do capítulo: cada seção deve explicar a parte correspondente do diagrama, sem reescrever o desenho em texto.
- Estrutura sugerida: Arquitetura | Formato pacote | Núcleo TransceiverBase | Batching/TX | Recepção/RX | Broadcast | Multicast | Integração teste (WcanTest + UART).
- Não inventar resultados numéricos; remeter ao capítulo de Resultados.
- Manter citações existentes onde ainda fizer sentido; adicionar \cite{espressif_espnow...} se citar payload.
- Não alterar outros capítulos.

Entregue o .tex completo atualizado e lista de \label{} criados.
```

**Suas ações:** usar `thesis/images/tx_path.svg` e `thesis/images/rx_path.drawio.svg`; não criar novas figuras para Desenvolvimento salvo se o PDF ficar ilegível.

---

## Passo 2 — Métodos

**Arquivo alvo:** `thesis/tex/capitulos/metodos.tex`  
**Dados:** `transmitter/testing/results/final/run_plan.json`, `analysis_summary.txt`, `summary.csv`, `transmitter/testing/tests.yaml`, `boards.yaml`, `README.md`, `analysis.py`

**Prompt:**

```
Atualize thesis/tex/capitulos/metodos.tex em português para refletir a campanha FINAL.

Fontes:
- run_plan.json em transmitter/testing/results/final/ (settings: repeats, duration, transports, measure)
- analysis_summary.txt e summary.csv na mesma pasta
- transmitter/testing/tests.yaml (suítes: baseline, multiple, real_time, active_filter, mixed_frequency)
- transmitter/testing/README.md, test_runner.py, analysis.py
- Código de teste: wcan_test/WcanTest.cpp (WCAN_CFG_WAIT v=1, WCAN_CFG_ACK, WCAN_TEST_*)
- main/main.cpp, RampCanSensor — NÃO citar runtime_config.cpp, sensor_app.hpp, receiver_app.hpp (não existem)

Corrigir:
- Repetições e totais conforme run_plan (ex.: repeats: 2, 684 testes na campanha final — confirmar nos artefatos).
- multiple: 3 CAN IDs por sensor (tests.yaml), não 4.
- Duas builds de firmware (BROADCAST / MULTICAST) + measure.
- ESP32 vs ESP32-C3: C3 só sensor, receptor só ESP32.
- Métricas: WCAN_SENSOR_END, R(RANGE), P(FAIL), S(FAIL), WCAN_BATCH, WCAN_MEASURES; remover CAN_PROC_*, LAT_RTT, LAT_CB, linhas TASK/hwm se não existirem nos logs atuais.
- Critério broadcast: ≥1 receptor esperado; par a par informativo.
- Tabela tab:suites-executadas alinhada a tests.yaml.

Apêndice: apontar pasta final/ como artefatos (atualizar thesis/tex/appendix/full_run.tex se necessário para results/final).

Não escrever análise de resultados (só metodologia). Não alterar desenvolvimento.tex.
```

**Suas ações:** confirmar que `full_run.tex` ou apêndice cita `results/final/`; anexar `run_plan.json` ao ZIP da entrega se a banca pedir.

---

## Passo 3 — Resultados (análise + redação)

**Arquivos alvo:** `thesis/tex/capitulos/resultados_testes.tex`, opcionalmente gráficos em `thesis/images/`, `thesis/tex/appendix/full_run.tex`, `thesis/thesis.tex` (resumo)

**Dados:** `transmitter/testing/results/final/` (toda a árvore), `analysis_summary.txt`, `summary.csv`

**Prompt:**

```
Tarefa em duas fases: (A) analisar dados; (B) reescrever resultados_testes.tex.

Dados: transmitter/testing/results/final/
- analysis_summary.txt, summary.csv, run_plan.json
- Árvore: {broadcast|multicast}/{suite}/{freq|mixed}/test_*S-*R_rep*/
- Logs: sensor_*, receiver_*, scenario.json

Ferramentas (rodar você mesmo):
- uv run python transmitter/testing/analysis.py transmitter/testing/results/final/<caminho_teste> (amostra de falhas)
- Revisar analysis_summary.txt e summary.csv já gerados
- Opcional: aggregate_thesis_data.py se precisar CSV consolidado

(A) ANÁLISE — produzir memo estruturado (markdown) com:
1. Totais: testes, aprovados, taxa; topologias aprovadas (summary já diz 453/684, 249/354 — revalidar).
2. Tabela por suíte × transporte: aprovados/total; destacar baseline, multiple, real_time, active_filter, mixed_frequency.
3. Efeito linger: real_time vs baseline/multiple.
4. Broadcast vs multicast sob carga (multiple, 750–1000 Hz).
5. active_filter: provar que filtro funciona (falhas = carga, não lógica).
6. Top 5 falhas representativas: caminho da pasta, trecho de log (P(FAIL), ESP_ERR_ESPNOW_NO_MEM, crash), classificação (entrega/transporte/heap/crash/freq).
7. Limitações: bancada USB, 2 repetições, ESP32-C3 só sensor.
8. Rastreabilidade: preencher linhas para tab:rastreabilidade-requisitos com evidência desta campanha.

(B) LaTeX — reescrever thesis/tex/capitulos/resultados_testes.tex em português:
- Substituir TODOS os números antigos (full_run 1116/847 etc.) pelos da campanha final.
- Manter estrutura: visão geral, batching, múltiplos fluxos, multicast, limitações, rastreabilidade, conclusão dos resultados.
- Tom defensável: broadcast+batching como melhor candidato; multiple como stress; multicast limitações sob carga.
- Tabelas LaTeX claras; sem inventar gráficos — só incluir se gerar a partir dos dados.
- Atualizar thesis/tex/appendix/full_run.tex → apontar results/final e totais corretos.
- Sugerir alterações ao Resumo em thesis/thesis.tex (bullet points, sem reescrever o arquivo inteiro salvo pedido).

Idioma: português. Não alterar metodos.tex nem desenvolvimento.tex neste passo.
```

**Suas ações:** garantir que `analysis_summary.txt` reflete 100% da pasta `final/`; se re-rodar testes, não misturar pastas.

---

## Passo 4 — Conclusão

**Arquivo:** `thesis/tex/capitulos/conclusao.tex`  
**Depende de:** Passo 3 concluído

**Prompt:**

```
Reescreva thesis/tex/capitulos/conclusao.tex em português com base nos resultados da campanha final (transmitter/testing/results/final/) e no capítulo resultados_testes.tex já atualizado.

Incluir:
- Objetivo cumprido em bancada; WCAN viável com ressalvas.
- Confirmação: broadcast + linger_ms como recomendação prática; batching essencial (real_time).
- Multicast: potencial vs limites (memória, carga) observados na campanha final — usar números do cap. resultados.
- Semântica primeiro ACK vs entrega a todos os receptores.
- Limitações e trabalhos futuros: veículo, TWAI produção, management_task/multicast, pilha/heap.
- Remover referências a filas genéricas onde o design atual usa rings; alinhar com desenvolvimento.tex.

Não alterar introdução nem métodos. Propor 2–3 frases para o Resumo (thesis.tex) se conclusão mudar mensagem principal.
```

---

## Passo 5 — Introdução

**Arquivo:** `thesis/tex/capitulos/introducao.tex` (hoje incompleto — só contexto FSAE)

**Prompt:**

```
Complete e atualize thesis/tex/capitulos/introducao.tex em português.

Manter: seções existentes sobre FSAE e aquisição de dados (revisar leve se necessário).

Adicionar:
1. Problema e motivação (1–2 parágrafos, remeter cap. Problema).
2. Objetivo geral e 4–5 objetivos específicos (implementar WCAN, duas variantes, campanha automatizada, avaliar em bancada).
3. Contribuições (protocolo/firmware, automação de testes, evidência experimental).
4. Organização do trabalho — um parágrafo por capítulo: Problema, Soluções, Requisitos, Framework, Desenvolvimento, Métodos, Resultados (campanha final em results/final), Conclusão, Apêndice.
5. Delimitação: bancada, não pista; não substitui CAN de controle.

Síntese dos resultados (3–5 frases) com números da campanha final — ler resultados_testes.tex atualizado, NÃO reanalisar logs.

Não usar \emph{} em palavras portuguesas. Inglês técnico sem ênfase ou com \textit{} apenas se padrão do template.

Não alterar outros capítulos.
```

---

## Passo 6 — `\emph{}` e inglês

**Escopo:** `thesis/tex/capitulos/*.tex`, `thesis/thesis.tex`, opcional `thesis/tex/appendix/*.tex`

**Prompt:**

```
Revise todos os arquivos .tex em thesis/tex/capitulos/, thesis/thesis.tex e thesis/tex/appendix/ para uso correto de ênfase:

Regra ICMC/ABNT comum: \emph{} para termos estrangeiros destacados ou ênfase forte; evitar \emph{} em palavras portuguesas.
- Remover \emph{} de termos já portugueses ou integrados.
- Termos técnicos em inglês (ESP-NOW, broadcast, multicast, firmware, etc.): usar \textit{} ou texto normal, consistente em todo o documento — escolher UMA convenção e aplicar.
- Não alterar conteúdo técnico nem números; só formatação de ênfase e pequenas correções ortográficas óbvias.

Liste alterações por arquivo. Não reescrever capítulos inteiros.
```

---

## Checklist final antes de entregar PDF

- [ ] `desenvolvimento.tex` sem referências ao código antigo
- [ ] Figuras legíveis em PDF (testar `\includegraphics`)
- [ ] `metodos.tex` com métricas e nomes de log corretos
- [ ] `resultados_testes.tex` só com números de `results/final/`
- [ ] Resumo (`thesis.tex`) alinhado à conclusão
- [ ] Apêndice aponta para `results/final/`
- [ ] Compilar: `pdflatex` / script do template sem erros de referência
- [ ] Lista de figuras e tabelas atualizada

---

## Ordem e dependências

```
Passo 0 (você: diagrama + exports)
    ↓
Passo 1 Desenvolvimento
    ↓
Passo 2 Métodos          ← pode paralelizar com 1 se quiser
    ↓
Passo 3 Resultados       ← precisa dados final/ prontos
    ↓
Passo 4 Conclusão
    ↓
Passo 5 Introdução
    ↓
Passo 6 emph (em qualquer momento após textos estáveis; ideal por último)
```

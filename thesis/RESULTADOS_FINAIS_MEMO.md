# Memo de Análise da Campanha Final

Este memo registra a análise de apoio ao capítulo de resultados. A base analisada foi `transmitter/testing/results/final/`, incluindo `run_plan.json`, `summary.csv`, `analysis_summary.txt`, os `analysis.txt` por execução, `scenario.json` e logs seriais de sensores e receptores.

## 1. Consistência e Totais

- `run_plan.json`: 684 execuções planejadas.
- `summary.csv`: 684 linhas, sendo 668 `OK` e 16 `MONITOR_ERROR`.
- `analysis.txt`: 684 arquivos encontrados, um por execução.
- Resultado bruto do analisador: 453/684 execuções aprovadas, taxa de 66,2%.
- Resultado por topologia: 249/354 topologias com ao menos uma repetição aprovada.
- Base útil de interpretação do protocolo: 453/679, removendo somente 5 quebras de estatísticas/log serial.

A campanha final é tratada como a única campanha experimental da tese. Não há comparação com testes anteriores no texto final.

## 2. Base Útil e Quebras de Estatística

Cinco reprovações foram separadas da interpretação de protocolo porque a falha está na emissão/parsing das estatísticas finais, não em entrega comprovada:

| Caminho | Sintoma | Classificação |
|---|---|---|
| `multicast/active_filter/100Hz/test_4S-1R_rep1` | `WCAN_MEASURES` com campo colado: `530747nt_total=272` | Corrupção de linha de estatística. |
| `multicast/active_filter/750Hz/test_3S-2R_rep0` | `WCAN_SENSOR_END generated750.19` e `WCAN_MEASURES elapsed_msfree=...` | Corrupção/truncamento de marcadores finais. |
| `multicast/active_filter/750Hz/test_3S-1R_rep0` | `1240925packets_sent_total=296` | Corrupção de linha de estatística. |
| `broadcast/baseline/750Hz/test_3S-2R_rep1` | `WCAN_SENSOR_END` perdeu o marcador e foi colado ao final de `WCAN_BATCH` | Corrupção de log final. |
| `broadcast/baseline/200Hz/test_2S-2R_rep0` | `WCAN_BATCH ... avg_qu generated=5998 ...` e campo heap truncado | Corrupção de log final. |

Não foram encontradas assinaturas de `panic`, `Guru Meditation`, `Backtrace` ou `abort()` nesses cinco casos. A evidência aponta para fragilidade da instrumentação textual por UART ou concorrência de `printf`, não para crash de firmware. Mesmo assim, isso é uma limitação real da bancada: o sistema de medição precisa ser mais robusto em uma campanha futura.

Outros logs incompletos foram mantidos como falha:

- `multicast/real_time` em 750 Hz e 1000 Hz: milhares de `ESP_ERR_ESPNOW_INTERNAL`, `S(FAIL)` e timeout do monitor. Isso é saturação real do caminho de envio.
- `broadcast/real_time/200Hz/test_3S-1R_rep1`: uma placa ficou em `WCAN_CFG_WAIT`; é falha da bancada/execução, não estatística.

## 3. Resultado por Suite e Transporte

| Suite | Transporte | Bruto | Base útil | Leitura |
|---|---:|---:|---:|---|
| multiple | BROADCAST | 34/90 | 34/90 | Caso crítico da tese; ACK/retry limita escala. |
| multiple | MULTICAST | 90/90 | 90/90 | Melhor resultado para múltiplos CAN IDs por sensor. |
| baseline | BROADCAST | 70/90 | 70/88 | Controle de um CAN ID; duas falhas eram estatísticas. |
| baseline | MULTICAST | 90/90 | 90/90 | Controle estável. |
| real_time | BROADCAST | 18/90 | 18/90 | Sem batching, falha acima de baixa frequência. |
| real_time | MULTICAST | 20/90 | 20/90 | Sem batching, satura driver/airtime. |
| active_filter | BROADCAST | 52/60 | 52/60 | Filtro correto, falhas de carga/entrega. |
| active_filter | MULTICAST | 57/60 | 57/57 | Três falhas brutas eram estatísticas; filtro validado. |
| mixed_frequency | BROADCAST | 10/12 | 10/12 | Heterogeneidade funciona, falha com carga densa. |
| mixed_frequency | MULTICAST | 12/12 | 12/12 | Todas passaram. |

## 4. Resultado Principal: Multiple

`multiple` é o resultado central porque o uso esperado raramente terá um único CAN ID por sensor. Cada sensor publicou três CAN IDs.

- `multiple` broadcast: 34/90 no total; 3/36 em 750-1000 Hz.
- `multiple` multicast: 90/90 no total; 36/36 em 750-1000 Hz.

No broadcast, a tarefa de retry envia o lote, espera callback de rádio, aguarda ACK de aplicação em `CONTROL_ID` e retransmite se o ACK não chegar. Isso oferece semântica mais forte, mas consome rádio e fila. Em `broadcast/multiple/1000Hz/test_4S-1R_rep0`, a execução teve 49,35% de entrega, 180.783 perdas internas, 172.905 `S(FAIL)` e espera máxima de fila de 3,83 s.

No multicast, a assinatura por CAN ID remove ACK de aplicação do caminho de dados. O callback confirma o caminho MAC/radio, e a entrega de aplicação é validada depois pelos `R(RANGE)` dos receptores. Essa troca foi a melhor para vazão na campanha.

## 5. Airtime e `ESP_ERR_ESPNOW_INTERNAL`

Agregados de airtime por análise:

| Suite/transporte | Média | P95 | Máximo |
|---|---:|---:|---:|
| multiple BROADCAST | 13,9% | 30,2% | 60,2% |
| multiple MULTICAST | 7,5% | 32,7% | 50,3% |
| real_time BROADCAST | 20,3% | 49,7% | 60,3% |
| real_time MULTICAST | 20,0% | 96,1% | 99,8% |

`ESP_ERR_ESPNOW_INTERNAL` é um erro interno retornado por `esp_now_send` antes do callback. Nos logs, ele aparece quando a taxa de chamadas ao driver fica alta demais, especialmente em `real_time` multicast. A documentação do ESP-NOW alerta que intervalos curtos entre envios podem causar desordem nos callbacks e recomenda aguardar o callback anterior para cargas grandes. Na campanha, o problema não foi apenas físico: o mesmo rádio sustenta 1000 Hz em `multiple` multicast com batching. A causa imediata é o desenho sem batching, que transforma frequência de amostra em frequência de pacote. O limite físico aparece como airtime e contenção do meio.

## 6. `P(FAIL)` com Entrega 100%

`broadcast/multiple/10Hz/test_4S-1R_rep1` passou com 100% de entrega interna e 685 `P(FAIL)`.

Isso não é contradição porque `P(FAIL)` não mede diretamente amostras ausentes. Ele mede lotes cujo retry task não recebeu ACK de aplicação dentro das tentativas. Apenas 10 desses `P(FAIL)` estavam na borda final. Os demais indicam ACK perdido/atrasado ou lote recuperado por retransmissão posterior; a entrega foi considerada completa porque todos os contadores esperados apareceram nos `R(RANGE)` dos receptores. Portanto, `P(FAIL)` é indicador de pressão do caminho de confirmação, não sinônimo de perda de aplicação.

## 7. Active Filter

Não houve entrega de CAN ID inesperado fora da allowlist. Após remover estatísticas corrompidas, `active_filter` multicast fica em 57/57.

O caso `broadcast/active_filter/1000Hz/test_2S-1R_rep1` não deve ser descrito como surpresa: se um fluxo broadcast não tem receptor interessado, ele naturalmente não recebe ACK, consome retries e depois descarta dados. Isso não é perda de aplicação porque o cenário não esperava receptor para aquele fluxo; é custo esperado do broadcast quando o emissor transmite algo que ninguém deve aceitar.

## 8. Quase Aprovações e Bordas

Quase aprovações importantes:

- `multicast/real_time/100Hz/test_4S-1R_rep1`: 99,99% de entrega, uma perda interna, sem `S(FAIL)`.
- `broadcast/baseline/750Hz/test_3S-1R_rep0`: 99,99% de entrega, 7 perdas internas.
- `broadcast/multiple/100Hz/test_1S-1R_rep1`: 99,98% de entrega, 2 perdas internas.

Perdas de borda são prefixos antes da primeira amostra recebida ou sufixos depois da última. Elas não entram na taxa interna porque normalmente refletem start/stop, drain do linger ou fim do monitor. São importantes se o sistema final exigir fechamento exato da janela de aquisição.

## 9. Rastreabilidade

| Requisito | Evidência | Estado |
|---|---|---|
| Desempenho e latência | Multicast com batching chegou a 1000 Hz; preservação temporal é robusta para frequência fixa ou até 100 Hz, mas acima disso depende de batching. Sem batching, `real_time` ficou em 38/180. | Parcial. |
| Conexão instantânea | Broadcast envia imediatamente; multicast começa em broadcast quando não há assinantes e só muda para peers cadastrados após os anúncios. | Atendido em bancada. |
| Múltiplas conexões | Topologias simultâneas até 4S-1R, 3S-2R e 2S-3R; falhas são de carga/transporte, não de suporte a múltiplos nós. | Atendido em bancada. |
| Transparência CAN | Mesmo CAN ID preservado, fluxos separados por identificador e ACK mantido ao entregar no barramento; CRC/ACK elétrico pertencem ao enlace CAN cabeado. | Atendido por projeto. |
| Físico e custo | Nó sensor com ESP32 compacto e barato, processamento leve e alimentação pequena, incluindo bateria reutilizada; falta ensaio no carro. | Atendido em bancada. |
| Modularidade | Builds broadcast/multicast, papéis configuráveis, filtros, frequências mistas e topologias com novos nós exercitados. | Atendido em bancada. |

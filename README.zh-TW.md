# Linux Resource Learning

**ScxSprout：以 C/eBPF 實作、在 eBPF 內訓練與推論的小型 sched_ext CPU 排程器。**

[English](README.md) · [部署與重現](docs/DEPLOYMENT.md) · [Phase 1 結果](docs/PHASE1_RESULTS.md)

**目前階段：Phase 1 已實作，並通過實際 Linux 核心部署驗證。**
2026-10-09 的 QEMU 測試完成編譯、BPF verifier 驗證、sched_ext 註冊、真實工作
執行、核心內模型更新，以及預測對有界限時間片的影響。正常卸載、訊號與程序終止、
SysRq 中止、watchdog 回復也通過。這些結果證明此環境中的架構可行性；
**目前不宣稱效能改善。**

## 已完成的功能

- 參考官方 sched_ext C 範例，保留全域 weighted virtual-time 排序，並以
  `sum_exec_runtime` 計算實際 CPU 執行量。
- 跨搶占累積執行區段；只有經確認的喚醒至睡眠事件產生完成標籤，持續執行的
  CPU 控制工作不產生完成標籤。
- 每個工作使用五個因果特徵與 Q16 定點數線性模型，在 eBPF callback 內進行
  有界限、誤差裁切的 LMS 更新與推論。
- `fixed` 使用固定時間片；`shadow` 訓練與推論但保留固定時間片；`online`
  在八個有效標籤後，讓預測影響 0.5–5 ms 的派送時間片。
- C/libbpf loader 執行相容性檢查、限時載入、JSONL 遙測與卸載；真實 CPU
  工作負載輸出 CPU 時間、喚醒延遲、完成延遲及完成工作量。

預測不改變派送排序或取代實際 CPU 使用量計算。預測已耗盡或無效時，使用固定
時間片。這個版本優先驗證部署、學習與回復，尚非正式生產版本。

## 實際測試環境與證據

測試使用 **Ubuntu 24.04.5 ARM64**、**Linux `7.0.0-38-generic`**、
**QEMU 10.2.1 與 HVF 硬體加速**，主機為 Apple Silicon macOS；客體配置
四個 vCPU、4 GiB RAM，並檢查 sched_ext、BPF、BTF 設定與實際介面。

線上模式的證據測試記錄 **1,345 次模型更新、1,355 個自適應執行區段**；
shadow 模式更新 1,348 次，fixed 模式更新為零。測試核對派送時間片與真實
`running` callback 觀察值，並以刻意停止派送的 SCHED_EXT 工作驗證 watchdog。

小型基準比較涵蓋 Linux 預設排程器、官方非 ML `scx_simple`、fixed 與 online
ScxSprout，各進行三次、每次四秒。短時間 VM 測量只建立可重現的基準，不能據此
宣稱效能提升或跨硬體的排程品質。各次試驗完成延遲 p95/p99 的中位數如下：

| 排程器 | p95（ms） | p99（ms） |
| --- | --- | --- |
| Linux 預設 | 3.259 | 3.350 |
| 官方 `scx_simple` | 20.016 | 20.128 |
| ScxSprout fixed | 2.015 | 2.031 |
| ScxSprout online | 2.023 | 3.996 |

這次短時間比較中，online 的 p99 高於 fixed 與 Linux 預設，尚未證明 ML 帶來改善。
請查看[完整結果與限制](docs/PHASE1_RESULTS.md)、
[機器可讀驗證](results/phase1/validation.json)與[環境記錄](results/phase1/environment.txt)。

## 編譯與重現

主機可執行模型與文件檢查：

```sh
git clone https://github.com/Denny-Lin/linux-resource-learning.git
cd linux-resource-learning
make test-model
python3 tests/check_docs.py
```

實際排程器必須在相容的 Linux 核心上載入。請依照[部署指南](docs/DEPLOYMENT.md)
建立 QEMU 客體、安裝依賴並使用運行中核心的 BTF 編譯。Docker 共享主機核心，
不能取代 QEMU 部署驗證。

[終端範例](examples/README.md)、[原始碼說明](src/README.md)、
[模型測試](tests/README.md)與[工作負載說明](benchmarks/README.md)提供目前介面。
下一步是較長的隨機化比較、模型預測誤差與成本分析、不同硬體驗證，以及 migration、
affinity、公平性與飢餓問題的壓力測試。貢獻方式見 [CONTRIBUTING](CONTRIBUTING.md)。

## 原始研究與授權

Phase 0 的[研究與既有工作](docs/planning/PRIOR_ART.md)、
[六個方向比較](docs/planning/PROJECT_COMPARISON.md)、
[架構](docs/planning/ARCHITECTURE.md)、[建議](docs/planning/RECOMMENDATION.md)、
[路線](docs/planning/ROADMAP.md)與[實驗計畫](docs/planning/BENCHMARK_PLAN.md)
保留原始提案與估算；目前實作狀態以部署指南和 Phase 1 結果為準。
既有工作已探索學習式排程與記憶體管理，此專案不宣稱首創。記憶體回收控制器仍為
研究備選方向。

原創文件、模型與工作負載採 [MIT](LICENSE)；由官方範例衍生的排程器、loader、
相關建置腳本與 vendored 材料遵循各自的 GPL 告示。請參閱
[THIRD_PARTY](THIRD_PARTY.md) 的來源、固定版本與授權界線。

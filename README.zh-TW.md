# Lertick — Linux 自我學習 CPU 排程器

**Lertick 是實驗性的 Linux CPU 排程器，在 eBPF 內根據工作的歷史 CPU burst 線上學習，並透過 sched_ext 選擇有界限的時間片。**

[English](README.md) · [部署與重現](docs/DEPLOYMENT.md) · [Phase 1 結果](docs/PHASE1_RESULTS.md) · [命名研究](docs/NAMING.md)

專案只研究 **使用 sched_ext 與 eBPF 的 Linux CPU 排程線上機器學習**，
包括 burst 預測、自適應時間片、排程決策、公平性、避免飢餓、低排程成本，以及
可重現的基準測試。訓練與推論都在核心內執行；userspace loader 只控制載入生命
週期並讀取遙測。

**目前為已通過實際 Linux 部署的 Phase 1 實驗原型，尚非正式生產排程器。**
Phase 2A 已在相同 Linux 客體通過正確性與安全測試；Phase 2B 完成 280 個基準
trial 與 80 個成本／觀測控制 trial。線性模型未顯示相對 fixed 的可靠吞吐量
增益，兩類 workload 的喚醒延遲反而退步。參閱
[Phase 2 結果](docs/PHASE2_RESULTS.md)。不宣稱普遍優於 Linux EEVDF 或其他排程器。

## 技術功能與架構

- 以 C/eBPF 實作 struct_ops，參考固定版本的官方 sched_ext C 範例。
- 保留共享 weighted virtual-time 佇列，以 `sum_exec_runtime` 計算實際 CPU
  使用量；預測不決定排序，也不取代公平性使用量計算。
- 跨搶占及時間片到期累積 CPU 執行量；只有經確認的喚醒至睡眠事件提供訓練
  標籤，初始載入與退出事件會排除。
- 每個工作使用五個因果特徵與 Q16 定點線性模型，在 eBPF callback 內執行
  有界限、誤差裁切的 LMS 更新與推論，不需要 userspace trainer。
- 八個有效標籤後可依預測選擇 0.5–5 ms 的時間片；無效或耗盡的預測回到固定值。
- 有限時間的 libbpf loader、核心相容性檢查、JSONL 遙測、真實 CPU 工作負載與
  獨立數值測試。

| 模式 | 模型 | 新派送時間片 |
| --- | --- | --- |
| `fixed` | 停用 | 預設固定 2 ms |
| `shadow` | eBPF 內訓練與推論 | 預設固定 2 ms |
| `online` | eBPF 內訓練與推論 | 暖身後依預測設定有界限時間片 |
| `ewma` | 歷史 burst EWMA；不訓練線性模型 | 暖身後依 EWMA 設定有界限時間片 |

架構流程為：喚醒時擷取歷史特徵 → eBPF 預測 → 暖身／固定值回退／時間片裁切
→ weighted virtual-time 派送 → 實際 CPU 執行量累積 → 確認睡眠後完成 burst
標籤 → eBPF 更新模型。特徵包含 bias、上一個 burst、burst EWMA、睡眠間隔與
佇列深度。預設追蹤 `lertick` 名稱前綴，亦可用 `--tracked-tgid PID` 指定程序。

## 編譯與部署

macOS 或 Linux 可執行可攜式檢查：

```sh
git clone https://github.com/Denny-Lin/lertick.git
cd lertick
make test-model
python3 tests/check_docs.py
```

實際排程器必須使用支援 `CONFIG_SCHED_CLASS_EXT`、BPF、BTF 的 Linux 核心。
請依照[部署指南](docs/DEPLOYMENT.md)建立 QEMU 客體，安裝固定依賴並依運行中
核心的 BTF 編譯。Docker 共享主機核心，不能取代 QEMU 驗證。

在準備好的 Ubuntu 客體中：

```sh
scripts/preflight.sh
scripts/build_libbpf.sh
export PKG_CONFIG_PATH="$PWD/build/deps/install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
make clean
make CLANG=clang-18
make test-model
sudo build/scx_lertick --check
sudo build/scx_lertick --mode online --duration 10 \
  --sample-ms 250 --stats-json /tmp/lertick.jsonl
```

[終端範例](examples/README.md)說明如何在載入期間執行具名 CPU 工作負載。

## Phase 1 實際結果與基準狀態

原始驗證於 2026-10-09 使用 Ubuntu 24.04.5 ARM64、Linux `7.0.0-38-generic`、
QEMU 10.2.1 與 HVF 硬體加速，配置四個 vCPU 及 4 GiB RAM。編譯、verifier、
註冊、真實執行、線上更新、時間片影響及回復皆通過。online 記錄 1,345 次更新與
1,355 個自適應執行區段；shadow 更新 1,348 次，fixed 不訓練。

這些原始資料在舊名稱 ScxSprout 下取得，所有 [Phase 1 證據](results/phase1/README.md)
逐位元保留；另以[重新命名回歸驗證](docs/REBRAND_VALIDATION.md)檢查新名稱。
重新命名不改變原有排程策略。

原始基準比較每個策略執行三次、每次四秒，burst 工作及兩個 CPU 競爭工作都固定
在客體 CPU 0。各次完成延遲百分位數的中位數如下：

| 排程器 | p95（ms） | p99（ms） |
| --- | ---: | ---: |
| Linux 預設 | 3.259 | 3.350 |
| 官方非 ML `scx_simple` | 20.016 | 20.128 |
| Lertick fixed 策略 | 2.015 | 2.031 |
| Lertick online 策略 | 2.023 | 3.996 |

**尚未證明 ML 效能改善。** 本次 online p99 高於 fixed 與 Linux 預設。
預設時間片差異、暖身、記錄成本、虛擬化及主機排程都限制短期測量的解讀。
[完整報告](docs/PHASE1_RESULTS.md)保留限制與實際數據。

## Phase 2 正確性與測量

Linux suite 通過 19 個 lifecycle case、156 個獨立 CPU clock／procfs 使用量
檢查、4,200-task churn、full preemption 下額外 8,000 個 fork/exec 子工作、
三個有限公平性 trial 及 15 個回復測試。持續執行
工作會把尚未 commit 的實際 CPU service 納入預算選擇；只有 stopping commit
使用量與公平性費用。保留 snapshot 的 LRU map 與真正的 task/model storage 分離。

[基準指南](benchmarks/README.md)比較 Linux 預設、官方 scx_simple、fixed、shadow、
EWMA、online 及相容的 scx_bpfland，涵蓋八種 CPU workload，保留原始資料、
隨機順序、核心 wake-to-switch 延遲、預測誤差、成本及不確定性。
360 個最終 trial 全數通過。online 相對 fixed 的核心喚醒 p99 在 mixed 與
multicore 分別增加約 214% 與 147%；所有 online/fixed 吞吐量區間都包含零變化。
每類有有效標籤的 workload，線性模型的同 episode MAE 均高於 EWMA。
兩類 workload 額外啟用核心計時器後，算術區段的平均值為每次線性更新約
24–26 ns、推論約 14–15 ns。客體計數器約每 41.7 ns 一個 tick；這些數值是
量化計時結果的平均，包含觀測成本，不代表單次操作有該精度，也不能解讀為
未加儀器的實體硬體成本。
參閱[正確性報告](docs/PHASE2_CORRECTNESS.md)與[實際結果](docs/PHASE2_RESULTS.md)。

## 安全與回復

只在可丟棄 VM 中進行 privileged 實驗，並保留獨立的主機控制路徑。loader 預設
30 秒，可設定 1–3600 秒；struct_ops link 不 pin。正常到期、SIGINT/SIGTERM
與程序死亡會關閉 link。SIGKILL、SysRq 與真實 stalled-task watchdog 回復也已
驗證，沒有安裝常駐服務。

卸載後確認：

```sh
cat /sys/kernel/sched_ext/state  # 預期 disabled
```

從獨立客體管理終端緊急中止 sched_ext：

```sh
printf S | sudo tee /proc/sysrq-trigger >/dev/null
cat /sys/kernel/sched_ext/state
```

必須使用大寫 `S`。watchdog 故障注入須搭配明確 VM 旗標、partial switching 與
單一 TGID，請使用部署指南的 runner，勿在生產機執行。

## 限制與專案範圍

目前只驗證一個 ARM64 客體／核心組合。Phase 2A 已修正持續執行工作的 pending
CPU service 扣除，並通過 lifecycle、4,200-task churn、affinity/migration、
有限公平性與重複回復測試。其他核心、實體硬體、長期生產行為、配置失敗注入、
PID 回繞重用及普遍公平／無飢餓保證仍未驗證。
時間片界限適用新派送預算，不保證精確實際執行時間，也不限制恢復時的剩餘預算。

記憶體配置最佳化、回收、DAMON/DAMOS、NUMA 記憶體放置及聯合 CPU／記憶體
管理不在專案範圍；相關發現僅保留於[背景研究檔案](docs/planning/README.md)，
不作為開發或備選方向。CPU 拓撲與 BPF map 用量仍屬排程器驗證資訊。

Phase 2 已批准；目前證據不支持開始 Phase 2C 參數最佳化，建議先重新檢驗
預測值映射到時間片的決策。
Phase 2 commits 只保留本機，推送仍需批准。[Phase 2 結果](docs/PHASE2_RESULTS.md)
另記實際測量，原 Phase 1 證據保持不變。參閱 [CONTRIBUTING](CONTRIBUTING.md)、
[安全政策](SECURITY.md)及[決策記錄](docs/DECISIONS.md)。

原創文件、模型與工作負載採 [MIT](LICENSE)；官方範例衍生的排程器、loader
及相關材料保留各自 GPL 告示。參閱 [THIRD_PARTY](THIRD_PARTY.md)。

# Linux Resource Learning

以小型、可量測的線上學習模型，探索 Linux CPU 排程與記憶體管理。

**目前階段：研究與規劃。尚未實作排程器，也沒有效能改善數據。**

第一個建議方向是 **ScxSprout (`scx-sprout`)**：以 C/eBPF 與 sched_ext 收集工作負載行為，使用定點數線性模型在線更新參數，預測 CPU 執行區段並調整有上下限的時間片。排序與 CPU 使用量計算仍由傳統公平機制負責。備選方向是針對 cgroup 的有界限記憶體回收控制器。

既有工作已探索 ML 排程、核心內推論與學習式快取回收；此專案不宣稱首創。重點是小型程式碼、清楚的學習機制、安全回復，以及能重現的比較實驗。

## 現在可以使用的內容

- [研究與既有工作](docs/planning/PRIOR_ART.md)
- [五種專案方向比較](docs/planning/PROJECT_COMPARISON.md)
- [架構](docs/planning/ARCHITECTURE.md)、[建議](docs/planning/RECOMMENDATION.md)與[開發路線](docs/planning/ROADMAP.md)
- [Ubuntu/QEMU 部署與回復計畫](docs/DEPLOYMENT.md)
- [效能測試計畫](docs/planning/BENCHMARK_PLAN.md)
- [開源策略](docs/planning/OPEN_SOURCE_STRATEGY.md)與[授權分析](docs/planning/LICENSE_ANALYSIS.md)

```sh
git clone https://github.com/Denny-Lin/linux-resource-learning.git
cd linux-resource-learning
python3 tests/check_docs.py
```

目前只需 Git 與 Python 3.10+ 即可執行文件檢查；此命令不操作核心。部署文件中的排程器指令是未來介面草案，尚不能執行。此專案尚未驗證任何 Ubuntu 或核心版本的實際相容性。

下一步是審查研究結論、模型標籤定義與測試門檻，核准方向後才開始實作。對外貢獻請參考 [CONTRIBUTING](CONTRIBUTING.md)。英文版 [README](README.md) 是完整入口。

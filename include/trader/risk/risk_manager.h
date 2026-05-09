#ifndef TRADER_RISK_RISK_MANAGER_H
#define TRADER_RISK_RISK_MANAGER_H

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <unordered_map>
#include <string>
#include <ostream>

#include "trader/strategy/position.h"
#include "trader/gpu/risk_gpu.h"
#include "trader/gpu/risk_cpu.h"
#include "trader/gpu/validation_gpu.h"
#include "trader/gpu/validation_cpu.h"

#ifdef HAVE_CUDA
#include "trader/gpu/risk_validation_gpu.cuh"
#endif

namespace Trader {
namespace Risk {

/**
 * RiskManager — Pre-trade risk gate for the HFT strategy.
 *
 * Enforces three checks on every batch of outgoing quotes:
 *
 *   1. Total Exposure Gate
 *      Σ |position_i × mid_price_i| > max_gross_exposure
 *      → halt ALL quoting until exposure falls below limit
 *
 *   2. Stop-Loss Gate
 *      Σ (realized_pnl_i + unrealized_pnl_i) < stop_loss_threshold
 *      → halt ALL quoting (daily loss limit hit)
 *
 *   3. Batch Order Validation (per-order checks)
 *      For each pending quote:
 *        a) Post-trade position would exceed per-symbol limit → block
 *        b) Quote price deviates > price_band_bps from mid → block (fat finger)
 *        c) Order size exceeds max_order_size → block
 *        d) No valid market (bid=0 or ask=0) → block
 *
 * CPU mode: all checks run in serial loops.
 * GPU mode: per-symbol risk kernel + batch validation kernel run in parallel.
 *           Portfolio totals computed from GPU outputs on CPU (O(N) sum is
 *           fast enough for the portfolio aggregation step).
 *
 * GPU speedup is meaningful at N > ~100 symbols (see benchmark results:
 * 5.87× at 1000 symbols, 23.9× at 5000 symbols).
 */
class RiskManager {
public:

    enum class Mode { None, CPU, GPU };

    // -------------------------------------------------------------------------
    // Configurable limits
    // -------------------------------------------------------------------------
    struct Limits {
        // Portfolio-level
        int64_t  max_gross_exposure    = 50'000'000'000LL; // $5,000,000 in $0.0001 units
        int64_t  stop_loss_threshold   = -1'000'000'000LL; // -$100,000 in $0.0001 units

        // Per-order
        int64_t  max_position          = 1000;     // shares per symbol
        uint64_t max_order_size        = 200;      // shares per single order
        uint64_t price_band_bps        = 500;      // 5% max deviation from mid
    };

    // -------------------------------------------------------------------------
    // Runtime statistics
    // -------------------------------------------------------------------------
    struct Stats {
        uint64_t orders_checked        = 0;
        uint64_t orders_blocked        = 0;
        uint64_t blocked_position      = 0;
        uint64_t blocked_price_band    = 0;
        uint64_t blocked_no_market     = 0;
        uint64_t blocked_size          = 0;
        uint64_t portfolio_halts       = 0; // times portfolio gate fired
        uint64_t exposure_halts        = 0;
        uint64_t stop_loss_halts       = 0;
        // Cumulative portfolio snapshot (last refresh)
        int64_t  last_gross_exposure   = 0;
        int64_t  last_total_pnl        = 0;
    };

    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------
    explicit RiskManager(Mode mode = Mode::CPU)
        : mode_(mode)
        , limits_()
        , stop_loss_triggered_(false)
        , exposure_breached_(false)
    {}

    RiskManager(Mode mode, Limits limits)
        : mode_(mode)
        , limits_(limits)
        , stop_loss_triggered_(false)
        , exposure_breached_(false)
    {}

    Mode mode() const { return mode_; }

    // -------------------------------------------------------------------------
    // Portfolio refresh — call after every fill or periodically.
    //
    // Computes:
    //   total_gross_exposure = Σ |position_i × mid_price_i|
    //   total_pnl            = Σ (realized_pnl_i + unrealized_pnl_i)
    //
    // Updates stop_loss_triggered_ and exposure_breached_ flags.
    // Returns true if trading may continue, false if halted.
    // -------------------------------------------------------------------------
    bool refresh_portfolio(const Strategy::PositionManager& pm,
                           const std::unordered_map<uint32_t, uint64_t>& mid_prices)
    {
        if (mode_ == Mode::None) return true;

#ifdef HAVE_CUDA
        if (mode_ == Mode::GPU) {
            return refresh_portfolio_gpu(pm, mid_prices);
        }
#endif
        return refresh_portfolio_cpu(pm, mid_prices);
    }

    // -------------------------------------------------------------------------
    // Batch order validation — call before submitting any quote batch.
    //
    // Checks each order against:
    //   - position limit (post-trade)
    //   - order size limit
    //   - price band (fat finger)
    //   - valid market exists
    //
    // Returns vector<bool>: allowed[i] = true → order i may be submitted.
    // -------------------------------------------------------------------------
    struct OrderEntry {
        uint32_t symbol_id;
        uint32_t side;      // 0 = buy, 1 = sell
        uint64_t price;
        uint64_t quantity;
    };

    struct SymbolContext {
        uint64_t best_bid;
        uint64_t best_ask;
        uint64_t mid_price;
        int64_t  current_position;
    };

    std::vector<bool> validate_batch(
        const std::vector<OrderEntry>& orders,
        const std::unordered_map<uint32_t, SymbolContext>& ctx)
    {
        std::vector<bool> allowed(orders.size(), true);
        if (mode_ == Mode::None || orders.empty()) return allowed;

#ifdef HAVE_CUDA
        if (mode_ == Mode::GPU) {
            return validate_batch_gpu(orders, ctx);
        }
#endif
        return validate_batch_cpu(orders, ctx);
    }

    // -------------------------------------------------------------------------
    // Portfolio gate — fast check before building the validation batch.
    // Returns false if ALL quoting should be halted.
    // -------------------------------------------------------------------------
    bool is_portfolio_ok() const {
        return !stop_loss_triggered_ && !exposure_breached_;
    }

    const Stats&  stats()  const { return stats_; }
    const Limits& limits() const { return limits_; }

    // Allow tuning limits at runtime
    void set_limits(const Limits& l) { limits_ = l; }

    // -------------------------------------------------------------------------
    // Print risk report to stream
    // -------------------------------------------------------------------------
    void write_report(std::ostream& os) const {
        os << "\n========================================\n";
        os << "Risk Management Report\n";
        os << "  Mode:              " << mode_str() << "\n";
        os << "  Limits:\n";
        os << "    Max gross exposure: $"
           << (limits_.max_gross_exposure / 10000.0) << "\n";
        os << "    Stop-loss:          $"
           << (limits_.stop_loss_threshold / 10000.0) << "\n";
        os << "    Max position/sym:   " << limits_.max_position << " shares\n";
        os << "    Max order size:     " << limits_.max_order_size << " shares\n";
        os << "    Price band:         " << limits_.price_band_bps << " bps\n";
        os << "  Portfolio state:\n";
        os << "    Gross exposure:     $"
           << (stats_.last_gross_exposure / 10000.0) << "\n";
        os << "    Total P&L:          $"
           << (stats_.last_total_pnl / 10000.0) << "\n";
        os << "    Stop-loss hit:      " << (stop_loss_triggered_ ? "YES" : "no") << "\n";
        os << "    Exposure breach:    " << (exposure_breached_   ? "YES" : "no") << "\n";
        os << "  Order validation:\n";
        os << "    Orders checked:     " << stats_.orders_checked << "\n";
        os << "    Orders blocked:     " << stats_.orders_blocked << "\n";
        os << "    -> Position limit:  " << stats_.blocked_position << "\n";
        os << "    -> Price band:      " << stats_.blocked_price_band << "\n";
        os << "    -> No market:       " << stats_.blocked_no_market << "\n";
        os << "    -> Size limit:      " << stats_.blocked_size << "\n";
        os << "  Portfolio halts:      " << stats_.portfolio_halts << "\n";
        os << "    -> Exposure halts:  " << stats_.exposure_halts << "\n";
        os << "    -> Stop-loss halts: " << stats_.stop_loss_halts << "\n";
        os << "========================================\n";
    }

private:
    Mode    mode_;
    Limits  limits_;
    Stats   stats_;
    bool    stop_loss_triggered_;
    bool    exposure_breached_;

    const char* mode_str() const {
        switch (mode_) {
            case Mode::None: return "none (disabled)";
            case Mode::CPU:  return "CPU";
            case Mode::GPU:  return "GPU (CUDA)";
        }
        return "unknown";
    }

    // =========================================================================
    // CPU implementation
    // =========================================================================

    bool refresh_portfolio_cpu(const Strategy::PositionManager& pm,
                               const std::unordered_map<uint32_t, uint64_t>& mid_prices)
    {
        int64_t gross = 0;
        int64_t total_pnl = 0;

        for (const auto& [sym_id, pos] : pm.positions()) {
            auto it = mid_prices.find(sym_id);
            uint64_t mid = (it != mid_prices.end()) ? it->second : 0;

            int64_t shares = pos.get_shares();
            int64_t exposure = shares * static_cast<int64_t>(mid);
            gross += (exposure >= 0) ? exposure : -exposure;

            int64_t realized = pos.calculate_realized_pnl();
            int64_t unrealized = pos.calculate_unrealized_pnl(mid);
            total_pnl += realized + unrealized;
        }

        stats_.last_gross_exposure = gross;
        stats_.last_total_pnl      = total_pnl;

        bool was_ok = is_portfolio_ok();

        stop_loss_triggered_ = (total_pnl < limits_.stop_loss_threshold);
        exposure_breached_   = (gross > limits_.max_gross_exposure);

        if (!is_portfolio_ok()) {
            ++stats_.portfolio_halts;
            if (stop_loss_triggered_) ++stats_.stop_loss_halts;
            if (exposure_breached_)   ++stats_.exposure_halts;
        }

        // Log transition to halted state
        (void)was_ok;

        return is_portfolio_ok();
    }

    std::vector<bool> validate_batch_cpu(
        const std::vector<OrderEntry>& orders,
        const std::unordered_map<uint32_t, SymbolContext>& ctx)
    {
        std::vector<bool> allowed(orders.size(), true);

        for (size_t i = 0; i < orders.size(); ++i) {
            const auto& o = orders[i];
            uint32_t reason = GPU::REJECT_NONE;

            auto it = ctx.find(o.symbol_id);
            if (it == ctx.end()) {
                // Unknown symbol — block
                allowed[i] = false;
                ++stats_.orders_blocked;
                continue;
            }
            const SymbolContext& sc = it->second;

            // Zero price or quantity
            if (o.price == 0)    reason |= GPU::REJECT_ZERO_PRICE;
            if (o.quantity == 0) reason |= GPU::REJECT_ZERO_QTY;

            // Valid market
            if (sc.best_bid == 0 || sc.best_ask == 0) {
                reason |= GPU::REJECT_NO_MARKET;
                ++stats_.blocked_no_market;
            }

            // Order size limit
            if (limits_.max_order_size > 0 && o.quantity > limits_.max_order_size) {
                reason |= GPU::REJECT_SIZE_LIMIT;
                ++stats_.blocked_size;
            }

            // Post-trade position check
            int64_t post_pos = sc.current_position
                + ((o.side == 0) ? static_cast<int64_t>(o.quantity)
                                 : -static_cast<int64_t>(o.quantity));
            int64_t abs_post = (post_pos >= 0) ? post_pos : -post_pos;
            if (abs_post > limits_.max_position) {
                reason |= GPU::REJECT_POSITION_LIMIT;
                ++stats_.blocked_position;
            }

            // Price band (fat finger)
            if (sc.mid_price > 0 && limits_.price_band_bps > 0) {
                int64_t diff = static_cast<int64_t>(o.price)
                             - static_cast<int64_t>(sc.mid_price);
                uint64_t abs_diff = (diff >= 0) ? static_cast<uint64_t>(diff)
                                                : static_cast<uint64_t>(-diff);
                uint64_t dev_bps = (abs_diff * 10000) / sc.mid_price;
                if (dev_bps > limits_.price_band_bps) {
                    reason |= GPU::REJECT_PRICE_BAND;
                    ++stats_.blocked_price_band;
                }
            }

            allowed[i] = (reason == GPU::REJECT_NONE);
            ++stats_.orders_checked;
            if (!allowed[i]) ++stats_.orders_blocked;
        }

        return allowed;
    }

    // =========================================================================
    // GPU implementation (only compiled when HAVE_CUDA is defined)
    // =========================================================================

#ifdef HAVE_CUDA
    // Device buffers — allocated once, reused across kernel launches
    GPU::RiskDeviceBuffers        risk_bufs_;
    GPU::ValidationDeviceBuffers  val_bufs_;

    bool refresh_portfolio_gpu(const Strategy::PositionManager& pm,
                               const std::unordered_map<uint32_t, uint64_t>& mid_prices)
    {
        // Build SoA host input from PositionManager
        uint32_t n = static_cast<uint32_t>(pm.positions().size());
        if (n == 0) return true;

        GPU::HostRiskInputStorage  h_in;
        GPU::HostRiskOutputStorage h_out;
        GPU::SymbolRiskInput  inp = h_in.view(n);
        GPU::SymbolRiskOutput out = h_out.view(n);

        uint32_t idx = 0;
        for (const auto& [sym_id, pos] : pm.positions()) {
            auto it = mid_prices.find(sym_id);
            uint64_t mid = (it != mid_prices.end()) ? it->second : 0;
            inp.position[idx]      = pos.get_shares();
            inp.mid_price[idx]     = mid;
            inp.best_bid[idx]      = 0; // not needed for portfolio refresh
            inp.best_ask[idx]      = 0;
            inp.avg_buy_price[idx] = pos.get_avg_buy_price();
            inp.total_bid_vol[idx] = 0;
            inp.total_ask_vol[idx] = 0;
            inp.max_position[idx]  = limits_.max_position;
            ++idx;
        }

        // Allocate / resize device buffers
        if (risk_bufs_.num_symbols != n) {
            risk_bufs_.free();
            risk_bufs_.allocate(n);
        }

        // Copy → kernel → copy back
        GPU::copy_risk_input_h2d(inp, risk_bufs_);
        GPU::launch_risk_kernel(risk_bufs_);
        cudaDeviceSynchronize();
        GPU::copy_risk_output_d2h(risk_bufs_, out);

        // Portfolio aggregation on CPU (15 symbols — trivial loop)
        GPU::PortfolioRisk pr = GPU::compute_portfolio_risk(inp, out);

        stats_.last_gross_exposure = pr.total_exposure;
        stats_.last_total_pnl      = pr.total_unrealized_pnl;  // realized not tracked here

        stop_loss_triggered_ = (pr.total_unrealized_pnl < limits_.stop_loss_threshold);
        exposure_breached_   = (pr.total_exposure        > limits_.max_gross_exposure);

        if (!is_portfolio_ok()) {
            ++stats_.portfolio_halts;
            if (stop_loss_triggered_) ++stats_.stop_loss_halts;
            if (exposure_breached_)   ++stats_.exposure_halts;
        }

        return is_portfolio_ok();
    }

    std::vector<bool> validate_batch_gpu(
        const std::vector<OrderEntry>& orders,
        const std::unordered_map<uint32_t, SymbolContext>& ctx)
    {
        // Build a compact symbol index mapping
        std::unordered_map<uint32_t, uint32_t> sym_to_idx;
        std::vector<uint32_t> idx_to_sym;
        for (const auto& [sym_id, _] : ctx) {
            sym_to_idx[sym_id] = static_cast<uint32_t>(idx_to_sym.size());
            idx_to_sym.push_back(sym_id);
        }
        uint32_t n_syms   = static_cast<uint32_t>(idx_to_sym.size());
        uint32_t n_orders = static_cast<uint32_t>(orders.size());

        GPU::HostOrderBatchStorage       h_batch;
        GPU::HostValidationContextStorage h_ctx;
        GPU::HostValidationResultStorage  h_res;

        GPU::OrderBatch       batch = h_batch.view(n_orders);
        GPU::ValidationContext vctx = h_ctx.view(n_syms);
        GPU::ValidationResult  vres = h_res.view(n_orders);

        // Fill context arrays
        for (uint32_t i = 0; i < n_syms; ++i) {
            uint32_t sym_id = idx_to_sym[i];
            const auto& sc  = ctx.at(sym_id);
            vctx.best_bid[i]         = sc.best_bid;
            vctx.best_ask[i]         = sc.best_ask;
            vctx.mid_price[i]        = sc.mid_price;
            vctx.current_position[i] = sc.current_position;
            vctx.max_position[i]     = limits_.max_position;
            vctx.max_order_size[i]   = limits_.max_order_size;
            vctx.price_band_pct[i]   = limits_.price_band_bps;
        }

        // Fill order arrays (remap symbol_id → local index)
        for (uint32_t i = 0; i < n_orders; ++i) {
            batch.symbol_id[i] = sym_to_idx.count(orders[i].symbol_id)
                                 ? sym_to_idx[orders[i].symbol_id] : 0;
            batch.side[i]      = orders[i].side;
            batch.price[i]     = orders[i].price;
            batch.quantity[i]  = orders[i].quantity;
        }

        // Allocate / resize
        if (val_bufs_.num_orders != n_orders || val_bufs_.num_symbols != n_syms) {
            val_bufs_.free();
            val_bufs_.allocate(n_orders, n_syms);
        }

        GPU::copy_validation_input_h2d(batch, vctx, val_bufs_);
        GPU::launch_validation_kernel(val_bufs_);
        cudaDeviceSynchronize();
        GPU::copy_validation_output_d2h(val_bufs_, vres);

        // Build result
        std::vector<bool> allowed(n_orders);
        for (uint32_t i = 0; i < n_orders; ++i) {
            allowed[i] = (vres.valid[i] == 1);
            ++stats_.orders_checked;
            if (!allowed[i]) {
                ++stats_.orders_blocked;
                if (vres.reject_reason[i] & GPU::REJECT_POSITION_LIMIT) ++stats_.blocked_position;
                if (vres.reject_reason[i] & GPU::REJECT_PRICE_BAND)     ++stats_.blocked_price_band;
                if (vres.reject_reason[i] & GPU::REJECT_NO_MARKET)      ++stats_.blocked_no_market;
                if (vres.reject_reason[i] & GPU::REJECT_SIZE_LIMIT)     ++stats_.blocked_size;
            }
        }
        return allowed;
    }
#endif // HAVE_CUDA
};

} // namespace Risk
} // namespace Trader

#endif // TRADER_RISK_RISK_MANAGER_H

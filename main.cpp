#include <iostream>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <gtest/gtest.h>
#include <benchmark/benchmark.h>
#include <immintrin.h>
#include <numeric>
#include <random>


constexpr size_t NUM_SLOTS = 524288;  // power of 2 
constexpr size_t SLOTS_PER_BLOCK = 32;
constexpr size_t NUM_BLOCKS = NUM_SLOTS / SLOTS_PER_BLOCK;

constexpr size_t SLOT_SIZE = 100;
constexpr size_t MAX_ORDERS = NUM_SLOTS * SLOT_SIZE;
constexpr size_t MAX_PRICE = 10238; // use multiple of 64 for uint64 bitmask - 2 (for MAX_PRICE + 2 size)

size_t next_block_slot_idx = 0;
const auto null_mask = _mm256_set1_epi8('\0');
auto cmp_mask = _mm256_set1_epi8('\0');
auto cmp_set_mask = _mm256_set1_epi8('\0');




struct book_entry_t
{
    size_t orders_idx;
    size_t next_price;  // book traversals are always in one dir
};

template <unsigned long N>
using i64bitmap_t = unsigned long long[N];

template <unsigned long N>
using i8bitmap_t = uint8_t[N];

template <unsigned long N>
struct BookT
{
    static constexpr auto bitmap_size = N/(sizeof(unsigned long long) * 8) + 31 + 31;
    book_entry_t prices[N]{};
    i64bitmap_t<bitmap_size> bitmap{};
    i8bitmap_t<bitmap_size> upper_bitmap{};
};

template <unsigned long N>
struct BookPairT
{
    static constexpr auto Size = N;
    using side_book_t = BookT<N>;
    side_book_t bids;
    side_book_t offers;
};

using book_t = BookPairT<MAX_PRICE + 2>;

template <typename T>
struct BookSize
{};

template <unsigned long N>
struct BookSize<BookPairT<N>>
{
    static constexpr auto size = N;
};

using map_t = std::unordered_map<std::string, book_t>;

enum class order_side : uint8_t
{
    BUY,
    SELL,
    BID,
    OFFER
};

struct order_entry_t
{
    unsigned int size;
    order_side side; 
    size_t next_idx;
// size = 16 (bec of alignment: 4 extra bytes if we want to put anything else in here) [multiple of 8 is good for traversal, 16 is very good]
};

struct input_order_t
{
    std::string symbol; 
    size_t size;
    size_t price;
    order_side side;
};

std::ostream& operator<<(std::ostream& os, input_order_t const& order)
{
    os << order.symbol; 
    os << ' ';
    os << static_cast<unsigned>(order.side);
    os << ' '; 
    os << order.size;
    os << ' '; 
    os << order.price;
    return os;
}

template <typename T, unsigned long N>
void print(T(&array)[N])
{
    for (auto i = 0; i < N; ++i)
        std::cout << array[i] << ", ";
    std::cout << std::endl;
}




uint8_t ArenaSlots[NUM_SLOTS];
order_entry_t OrdersArena[MAX_ORDERS];
map_t InstrumentBooks;
size_t positions[3];

void init()
{
    std::memset(&ArenaSlots[0], 0, NUM_SLOTS);
    std::fill_n(&OrdersArena[0], MAX_ORDERS, order_entry_t{});
    InstrumentBooks = map_t{};
    std::memset(&positions[0], 0UL, 3*sizeof(size_t));
}


const char order_side_strs[4][6]
{
    "BUY",
    "SELL",
    "BID",
    "OFFER"
};


struct BookPrinter
{
    static void print(map_t const& book)
    {
        for (const auto& symbol: book)
        {
            auto& bids = symbol.second.bids.prices;
            auto max = bids[MAX_PRICE + 1].next_price;
            while (max)
            {
                auto orders_idx = bids[max].orders_idx;
                for (;;)
                {
                    auto& order = OrdersArena[orders_idx];
                    std::cout << symbol.first << ' '
                              << order_side_strs[static_cast<unsigned>(order.side)] << ' '
                              << order.size << ' '
                              << max << std::endl;
                    if (order.next_idx == orders_idx)
                        break;
                    orders_idx = order.next_idx;
                }
                max = bids[max].next_price;
            }
            auto& offers = symbol.second.offers.prices;
            auto min = offers[0].next_price;
            if (min > 0)
                while (min < MAX_PRICE + 1)
                {
                    auto orders_idx = offers[min].orders_idx;
                    for (;;)
                    {
                        auto& order = OrdersArena[orders_idx];
                        std::cout << symbol.first << ' '
                                << order_side_strs[static_cast<unsigned>(order.side)] << ' '
                                << order.size << ' '
                                << min << std::endl;
                        if (order.next_idx == orders_idx)
                            break;
                        orders_idx = order.next_idx;
                    }
                    min = offers[min].next_price;
                }
        }
    }
};

class PositionTracker
{
    public:
        // static void add_pl(int side, int ls, size_t size, int spread) // for seller takes spread
        // {
        //     // ls == 1 = order is long
        //     //           (side+ls - 2) (2*ls - 1)
        //     //BUY    0 --> (0+1 - 2)(2*1 - 1) = -1
        //     //SELL   1 --> (1+0 - 2)(2*0 - 1) = 1
        //     //BID    2 --> (2+1 - 2)(2*1 - 1) = 1
        //     //OFFER  3 --> (3+0 - 2)(2*0 - 1) = -1


        //     // 2 - 
        //     //BUY    0 --> 2 - 0|1 = 1
        //     //SELL   1 --> 2 - 1|1 = 1
        //     //BID    2 --> 2 - 2|1 = 0



        //     positions[0] += (side+ls - 2)*(2*ls - 1) * size * spread;
        // }
        static void add_pl(size_t size, int spread)
        {
            positions[0] += size * spread;
        }
        static void add_position(int side, size_t size, size_t price)
        {
            positions[side+1] += (side < 2) * size * price;
            
        }
        static void remove_position(int side, size_t size, size_t price)
        {
            positions[side+1] -= size * price;
        }
};



class OrdersArenaAllocator
{
    public:
    // ~10ns per allocation, and goes around arena to reallocate filled order slots
        static size_t new_pos()
        {
// for first run-through do simple incr: extra cost every call after first runthrough if anticipate overflow
// if (next_block++ < NUM_BLOCKS)
//     return next_block * SLOTS_PER_BLOCK;
            const auto st_slot = next_block_slot_idx;
            do
            {
                auto vec = _mm256_loadu_ps(reinterpret_cast<float const*>(&ArenaSlots[next_block_slot_idx]));
                cmp_mask = _mm256_cmpeq_epi8(_mm256_castps_si256(vec), null_mask);
                auto mask = _mm256_movemask_epi8(cmp_mask);
                if (mask)
                    return __builtin_ffs(mask) + next_block_slot_idx; 
                next_block_slot_idx += SLOTS_PER_BLOCK;
            } while (next_block_slot_idx < NUM_SLOTS);
            next_block_slot_idx = 0;
            while (next_block_slot_idx < st_slot)
            {
                auto vec = _mm256_loadu_ps(reinterpret_cast<float const*>(&ArenaSlots[next_block_slot_idx]));
                cmp_mask = _mm256_cmpeq_epi8(_mm256_castps_si256(vec), null_mask);
                auto mask = _mm256_movemask_epi8(cmp_mask);
                if (mask)
                    return __builtin_ffs(mask) + next_block_slot_idx; 
                next_block_slot_idx += SLOTS_PER_BLOCK;
            }
            return 0;
        }
};


// ~10ns for < <--64--> > search, ~75ns for full 10000pt search; on average should be clustered 
// template <unsigned long N>
class BiLayerBitmapSeeker
{
    static constexpr auto max_ull = std::numeric_limits<unsigned long long>::max();
    public:
        template <unsigned long N>
        static size_t find_next(i64bitmap_t<N>& bitmap, i8bitmap_t<N>& upper, size_t st_bit)
        {
            auto st = st_bit / 64;
            auto rpos = __builtin_ffsll(bitmap[st + 31] & (max_ull << st_bit + 1 - st * 64));
            if (rpos)
                return rpos + st * 64;
            for (auto i = st + 31 + 1; i < N - 31; i += 32)
            {
                auto vec = _mm256_loadu_ps(reinterpret_cast<float const*>(&upper[i]));
                cmp_mask = _mm256_cmpgt_epi8(_mm256_castps_si256(vec), null_mask);
                auto mask = _mm256_movepi8_mask(cmp_mask);
                if (mask)
                {
                    auto idx = __builtin_ffs(mask) + i - 1;
                    return __builtin_ffsll(bitmap[idx]) + (idx - 31) * 64;
                }
            }
            return 0;
        }

        template <unsigned long N>
        static size_t find_prev(i64bitmap_t<N>& bitmap, i8bitmap_t<N>& upper, size_t st_bit)
        {
            auto st = st_bit / 64;
            auto offset = st * 64 + 64;
            auto lzs = __builtin_clzll(bitmap[st + 31] & (max_ull >> offset - st_bit));
            if (lzs - 64)
                return offset - lzs; 
            for (int i = st - 1; i > -1; i -= 32)  // st + 31 - 32
            {
                auto vec = _mm256_loadu_ps(reinterpret_cast<float const*>(&upper[i]));
                cmp_mask = _mm256_cmpgt_epi8(_mm256_castps_si256(vec), null_mask);
                auto mask = _mm256_movepi8_mask(cmp_mask);
                if (mask)
                {
                    auto idx = i + 32 - 1 - __builtin_clz(mask);
                    return (idx - 31) * 64 + 64 - __builtin_clzll(bitmap[idx]);
                }
            }
            return 0;
        }
};

// template <unsigned long N>
class BookBuilder
{
    private:
        using side_book_t = book_t::side_book_t;

        void fill(side_book_t& book, input_order_t& order, size_t MINMAX, int ls) const
        {
            auto& prices = book.prices;
            auto& minmax = prices[MINMAX].next_price;
            long spread;
            while ((spread = (order.price - minmax) * (2*ls - 1)) >= 0) // !!assumes order.price cannot be 0
            {
                auto& orders_idx = prices[minmax].orders_idx;
                for (;;)
                {
                    auto& book_order = OrdersArena[orders_idx];
                    // fill as much as available
                    auto trade_size = book_order.size < order.size ? book_order.size : order.size;
                    book_order.size -= trade_size;
                    order.size -= trade_size;
                    auto order_side = static_cast<int>(order.side);
                    auto book_side = static_cast<int>(book_order.side);
                    if (book_side + order_side == 3)
                        PositionTracker::add_pl(trade_size, spread);
                    if (book_side < 2)
                        PositionTracker::remove_position(book_side, trade_size, minmax);
                    if (book_order.size)
                        return;
                    --ArenaSlots[orders_idx/SLOT_SIZE];
                    orders_idx = book_order.next_idx;
                    if (!ArenaSlots[orders_idx/SLOT_SIZE]) 
                        break;
                    if (!order.size)
                        return;
                }
                auto bitmap_idx = minmax/64 + 31;
                book.bitmap[bitmap_idx] &= ~(1ULL << minmax - minmax/64 * 64);
                book.upper_bitmap[bitmap_idx] = book.bitmap[bitmap_idx] > 0;
                minmax = prices[minmax].next_price; // assumes no order > MAX_PRICE is entered
                if (!order.size)
                    return;
            }
        }
        void insert_short(side_book_t& book, input_order_t& order) const
        {
            auto& entry = book.prices[order.price];
            auto bitmap_idx = order.price/64 + 31;
            auto price_bit = 1ULL << order.price - order.price/64 * 64;
            if (!(book.bitmap[bitmap_idx] & price_bit)) // insert into prices
            {
                auto new_orders_pos = OrdersArenaAllocator::new_pos(); // set order_idx in prices[] for new price
                if (!new_orders_pos) [[unlikely]]
                    return;
                entry.orders_idx = (new_orders_pos - 1) * SLOT_SIZE;
                auto prev_price = BiLayerBitmapSeeker::find_prev(book.bitmap, book.upper_bitmap, order.price);
                if (prev_price) [[likely]]
                    --prev_price; // if prev_idx == 0, set min at prices[0]
                book.prices[prev_price].next_price = order.price;
                auto next_price = BiLayerBitmapSeeker::find_next(book.bitmap, book.upper_bitmap, order.price);
                if (!next_price--) [[unlikely]]
                {
                    next_price = MAX_PRICE + 1;
                    book.prices[next_price].next_price = order.price;
                }
                entry.next_price = next_price;
                // set bitmaps for new price
                book.bitmap[bitmap_idx] |= price_bit;
                book.upper_bitmap[bitmap_idx] = 1;
            }
            auto begin_idx = entry.orders_idx;
            auto last_idx = begin_idx + ArenaSlots[begin_idx/SLOT_SIZE]; // insert into ordersarena...
            auto end_idx = begin_idx/SLOT_SIZE * SLOT_SIZE + SLOT_SIZE;
            while (last_idx == end_idx)
            {
                begin_idx = OrdersArena[end_idx - 1].next_idx;
                if (begin_idx == end_idx - 1)
                {
                    auto next_idx = OrdersArenaAllocator::new_pos();
                    if (!next_idx--) [[unlikely]] // don't need to undo bitmaps bec if last_idx == end_idx --> price was there before
                        return;
                    ++ArenaSlots[next_idx];         
                    next_idx *= SLOT_SIZE;
                    OrdersArena[next_idx].size = order.size;
                    OrdersArena[next_idx].side = order.side;
                    OrdersArena[next_idx].next_idx = next_idx;
                    OrdersArena[end_idx - 1].next_idx = next_idx;
                    PositionTracker::add_position(static_cast<int>(order.side), order.size, order.price);
                    return;
                }
                last_idx = begin_idx + ArenaSlots[begin_idx/SLOT_SIZE];
                end_idx = begin_idx + SLOT_SIZE;
            }
            OrdersArena[last_idx].size = order.size;
            OrdersArena[last_idx].side = order.side;
            OrdersArena[last_idx].next_idx = last_idx;
            auto& count = ArenaSlots[last_idx/SLOT_SIZE];
            if (count++)
                OrdersArena[last_idx - 1].next_idx = last_idx;
            PositionTracker::add_position(static_cast<int>(order.side), order.size, order.price);
            return;
        }
        void insert_long(side_book_t& book, input_order_t& order) const
        {
            auto& entry = book.prices[order.price];
            auto bitmap_idx = order.price/64 + 31;
            auto price_bit = 1ULL << order.price - order.price/64 * 64;
            if (!(book.bitmap[bitmap_idx] & price_bit)) // insert into prices
            {
                auto new_orders_pos = OrdersArenaAllocator::new_pos(); // set order_idx in prices[] for new price
                if (!new_orders_pos) [[unlikely]]
                    return;
                entry.orders_idx = (new_orders_pos - 1) * SLOT_SIZE;
                auto prev_price = BiLayerBitmapSeeker::find_prev(book.bitmap, book.upper_bitmap, order.price);
                if (!prev_price--) [[unlikely]]
                {
                    prev_price = 0;
                    book.prices[prev_price].next_price = order.price;
                }
                entry.next_price = prev_price;
                auto next_price = BiLayerBitmapSeeker::find_next(book.bitmap, book.upper_bitmap, order.price);
                if (!next_price--) [[unlikely]]
                    next_price = MAX_PRICE + 1;
                book.prices[next_price].next_price = order.price;
                // set bitmaps for new price
                book.bitmap[bitmap_idx] |= price_bit;
                book.upper_bitmap[bitmap_idx] = 1;
            }
            auto begin_idx = entry.orders_idx;
            auto last_idx = begin_idx + ArenaSlots[begin_idx/SLOT_SIZE]; // insert into ordersarena...
            auto end_idx = begin_idx/SLOT_SIZE * SLOT_SIZE + SLOT_SIZE;
            while (last_idx == end_idx)
            {
                begin_idx = OrdersArena[end_idx - 1].next_idx;
                if (begin_idx == end_idx - 1)
                {
                    auto next_idx = OrdersArenaAllocator::new_pos();
                    if (!next_idx--) [[unlikely]] // don't need to undo bitmaps bec if last_idx == end_idx --> price was there before
                        return;
                    ++ArenaSlots[next_idx];         
                    next_idx *= SLOT_SIZE;
                    OrdersArena[next_idx].size = order.size;
                    OrdersArena[next_idx].side = order.side;
                    OrdersArena[next_idx].next_idx = next_idx;
                    OrdersArena[end_idx - 1].next_idx = next_idx;
                    PositionTracker::add_position(static_cast<int>(order.side), order.size, order.price);
                    return;
                }
                last_idx = begin_idx + ArenaSlots[begin_idx/SLOT_SIZE];
                end_idx = begin_idx + SLOT_SIZE;
            }
            OrdersArena[last_idx].size = order.size;
            OrdersArena[last_idx].side = order.side;
            OrdersArena[last_idx].next_idx = last_idx;
            auto& count = ArenaSlots[last_idx/SLOT_SIZE];
            if (count++)
                OrdersArena[last_idx - 1].next_idx = last_idx;
            PositionTracker::add_position(static_cast<int>(order.side), order.size, order.price);
            return;
        }
    public:
        void new_order(input_order_t order) const
        {
            auto& book = InstrumentBooks[order.symbol];
            if (static_cast<uint8_t>(order.side) & 1) // remove check completely (pass in order.side ^ 1) once done abstracting here
            {
                auto& fill_book = book.bids;
                if (fill_book.prices[MAX_PRICE + 1].next_price) [[likely]]
                    fill(fill_book, order, MAX_PRICE + 1, 0); 
                if (order.size)
                    insert_short(book.offers, order);
                return;
            }
            auto& fill_book = book.offers;
            if (fill_book.prices[0].next_price) [[likely]]
                fill(fill_book, order, 0, 1);
            if (order.size)
                insert_long(book.bids, order);
            return;
        }
};

class Gateway
{
    private:
        BookBuilder& book_builder;
        char order_side_strs[4][6]
        {
            "BUY",
            "SELL",
            "BID",
            "OFFER"
        };
    public:
        Gateway(BookBuilder& book_builder) : book_builder(book_builder)
        {}
        void process_stream(std::istream& is)
        {
            char side[6]{};
            char symbol[12]{}; // 11 capacity not too big for std::string small string optimization (15 bytes + \0)
            is >> symbol;
            while (is)
            {
                for (;;)
                {
                    input_order_t order;
                    order.symbol = symbol;
                    is >> side;
                    uint8_t i{};
                    for (; i < 4; ++i)
                        if (std::strcmp(side, order_side_strs[i]) == 0)
                            break;
                    order.side = static_cast<order_side>(i); 
                    is >> order.size;
                    is >> order.price;
                    book_builder.new_order(std::move(order)); // move to avoid copy 56 bytes
                    // BookPrinter::print(InstrumentBooks);
                    // print(positions);
                    if (is.peek() == '\n' || is.peek() == -1)
                        break;
                };
                is >> symbol;
            }
        }
};

static void BM_insert_orders(benchmark::State& state)
{
    auto orders = std::vector<std::string>
    {
                "TINYCORP SELL 27 1",
                "OTHER BID 5 20 OFFER 5 25",
                "MEDPHARMA BID 3 120 OFFER 7 150",
                "NEWFIRM BID 10 140 BID 7 150 OFFER 14 180",
                "TINYCORP BID 25 3 OFFER 25 6",
                "FASTAIR BID 21 65 OFFER 35 85",
                "FLYCARS BID 50 80 OFFER 100 90",
                "BIGBANK BID 200 13 OFFER 100 19",
                "REDCHIP BID 55 25 OFFER 80 30",
                "FASTAIR BUY 50 100",
                "CHEMCO SELL 100 67",
                "OTHER BUY 5 30",
                "REDCHIP SELL 5 30",
                "NEWFIRM BUY 2 200",
                "MEDPHARMA BUY 2 150",
                "BIGBANK SELL 50 11",
                "FLYCARS BUY 200 100",
                "CHEMCO BID 1000 77 OFFER 500 88"
    };
    auto builder = BookBuilder{};
    auto gateway = Gateway{builder};
    auto is = std::stringstream{};
    auto n = orders.size();
    for (auto i = 0; i < n; ++i)
        is << orders[i] << '\n';
    init();
    for (auto _ : state)
    {
        gateway.process_stream(is);
    }
}

BENCHMARK(BM_insert_orders);
 
BENCHMARK_MAIN();

// int main(int ac, char** av)
// {
//     auto builder = BookBuilder{};
//     auto gateway = Gateway{builder}; 
//     auto orders = std::vector<std::string>
//     {
//               "TINYCORP SELL 27 1",
//               "OTHER BID 5 20 OFFER 5 25",
//               "MEDPHARMA BID 3 120 OFFER 7 150",
//               "NEWFIRM BID 10 140 BID 7 150 OFFER 14 180",
//               "TINYCORP BID 25 3 OFFER 25 6",
//               "FASTAIR BID 21 65 OFFER 35 85",
//               "FLYCARS BID 50 80 OFFER 100 90",
//               "BIGBANK BID 200 13 OFFER 100 19",
//               "REDCHIP BID 55 25 OFFER 80 30",
//               "FASTAIR BUY 50 100",
//               "CHEMCO SELL 100 67",
//               "OTHER BUY 5 30",
//               "REDCHIP SELL 5 30",
//               "NEWFIRM BUY 2 200",
//               "MEDPHARMA BUY 2 150",
//               "BIGBANK SELL 50 11",
//               "FLYCARS BUY 200 100",
//               "CHEMCO BID 1000 77 OFFER 500 88"
//     };
//     auto builder = BookBuilder{};
//     auto gateway = Gateway{builder};
//     auto is = std::stringstream{};
//     auto n = orders.size();
//     for (auto i = 0; i < n; ++i)
//       is << orders[i] << '\n';
//     init();
//     gateway.process_stream(is);

//     return 0;
// }

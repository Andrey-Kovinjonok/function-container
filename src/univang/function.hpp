#pragma once
// std::function-like callable wrappers.
//============================================================================
#include <functional>

namespace univang {

// Function options.
enum class fn_opt {
    none = 0,
    copy = 1,
    move = 2,
    no_alloc = 4,
    once = 8 + 2, // +2 to ensure movable
    // Option combo's.
    copy_move = 3
};

inline constexpr fn_opt operator|(fn_opt lhs, fn_opt rhs) {
    return fn_opt(static_cast<int>(lhs) | static_cast<int>(rhs));
}
inline constexpr fn_opt operator&(fn_opt lhs, fn_opt rhs) {
    return fn_opt(static_cast<int>(lhs) & static_cast<int>(rhs));
}

template<class F, size_t Size, fn_opt Options>
class basic_function;

namespace detail {
namespace function {

// Default small-optimize size.
constexpr static size_t default_size = sizeof(void*) * 4;

// Optional specialization helpers.
//============================================================================
inline constexpr bool fn_opt_enabled(fn_opt mask, fn_opt opt) {
    return (mask & opt) == opt;
}

template<fn_opt Options, fn_opt CheckOption>
using enable_if_cpmove =
    typename std::enable_if<(Options & fn_opt::copy_move) == CheckOption>::type;

template<fn_opt Options>
using enable_if_once =
    typename std::enable_if<(Options & fn_opt::once) == fn_opt::once>::type;
template<fn_opt Options>
using enable_if_not_once =
    typename std::enable_if<(Options & fn_opt::once) != fn_opt::once>::type;

// Function invocation helper.
//============================================================================
template<class F, bool Local, bool IsConst, class R, class... Args>
struct fn_handler;

// Local storage.
template<class F, bool IsConst, class R, class... Args>
struct fn_handler<F, true, IsConst, R, Args...> {
    static R invoke(void* f, Args... args) {
        using Fp = typename std::conditional<IsConst, const F*, F*>::type;
        return (*static_cast<Fp>(f))(static_cast<Args&&>(args)...);
    }
};

// Dynamic storage.
template<class F, bool IsConst, class R, class... Args>
struct fn_handler<F, false, IsConst, R, Args...> {
    static R invoke(void* f, Args... args) {
        using Fp = typename std::conditional<IsConst, const F*, F*>::type;
        return (**static_cast<Fp*>(f))(static_cast<Args&&>(args)...);
    }
};

// Function operation (destruct/copy/move etc.).
//============================================================================
enum class exec_op {
    DESTRUCT,
    MOVE,
    COPY,
};

template<class F, bool LocalStorage, bool Movable, bool Copyable>
struct fn_manager;

// Local storage.
template<class F, bool Movable, bool Copyable>
struct fn_manager<F, true, Movable, Copyable> {
    static void move(void* src, void* dst, std::true_type /*tag*/) {
        ::new(dst) F(std::move(*static_cast<F*>(src)));
        static_cast<F*>(src)->~F();
    }
    static void move(void* /*src*/, void* /*dst*/, std::false_type /*tag*/) {
    }
    static void copy(void* src, void* dst, std::true_type /*tag*/) {
        ::new(dst) F(*static_cast<const F*>(src));
    }
    static void copy(void* /*src*/, void* /*dst*/, std::false_type /*tag*/) {
    }
    static void manage(exec_op op, void* src, void* dst) {
        switch(op) {
        case exec_op::DESTRUCT:
            static_cast<F*>(src)->~F();
            break;
        case exec_op::MOVE:
            move(src, dst, std::integral_constant<bool, Movable>());
            break;
        case exec_op::COPY:
            copy(src, dst, std::integral_constant<bool, Copyable>());
            break;
        }
    }
};

// Dynamic storage.
template<class F, bool Movable, bool Copyable>
struct fn_manager<F, false, Movable, Copyable> {
    static void move(void* src, void* dst, std::true_type /*tag*/) {
        F** src_fn = static_cast<F**>(src);
        F** dst_fn = static_cast<F**>(dst);
        *dst_fn = *src_fn;
        *src_fn = nullptr;
    }
    static void move(void* /*src*/, void* /*dst*/, std::false_type /*tag*/) {
    }
    static void copy(void* src, void* dst, std::true_type /*tag*/) {
        const F* src_fn = *static_cast<const F**>(src);
        F** dst_fn = static_cast<F**>(dst);
        *dst_fn = new F(*src_fn);
    }
    static void copy(void* /*src*/, void* /*dst*/, std::false_type /*tag*/) {
    }
    static void manage(exec_op op, void* src, void* dst) {
        switch(op) {
        case exec_op::DESTRUCT:
            delete *static_cast<F**>(src);
            break;
        case exec_op::MOVE:
            move(src, dst, std::integral_constant<bool, Movable>());
            break;
        case exec_op::COPY:
            copy(src, dst, std::integral_constant<bool, Copyable>());
            break;
        }
    }
};

// Most base function class.
//============================================================================
template<size_t Size, fn_opt Options, bool IsConst, class R, class... Args>
class function_data {
public:
    constexpr static bool is_const = IsConst;
    constexpr static bool is_copyable = fn_opt_enabled(Options, fn_opt::copy);
    constexpr static bool is_movable = fn_opt_enabled(Options, fn_opt::move);
    constexpr static bool no_alloc = fn_opt_enabled(Options, fn_opt::no_alloc);

    using result_type = R;

    constexpr function_data() noexcept
        : invoke_(&bad_call_), manage_(nullptr), data_null_state_() {
    }

    function_data(function_data&& rhs) noexcept
        : invoke_(rhs.invoke_), manage_(rhs.manage_) {
        if(manage_ == nullptr)
            return;
        rhs.manage_(exec_op::MOVE, &rhs.data_, &data_);
        rhs.default_construct_();
    }

    ~function_data() {
        if(manage_ != nullptr)
            manage_(exec_op::DESTRUCT, &data_, nullptr);
    }

    explicit operator bool() const {
        return manage_ != nullptr;
    }

protected:
    using storage_type = typename std::aligned_storage<Size>::type;
    using call_fn = R (*)(void*, Args...);
    using exec_fn = void (*)(exec_op, void*, void*);

    call_fn invoke_;
    exec_fn manage_;
    union {
        char data_null_state_;
        storage_type data_;
    };

    void* get_data_() const noexcept {
        return const_cast<void*>(static_cast<const void*>(&data_));
    }

    static R bad_call_(void* /*f*/, Args... /*args*/) {
        throw std::bad_function_call();
    }

    // Construct local.
    template<class F>
    void construct_(F&& f, std::true_type /*tag*/) {
        using functor_type = typename std::decay<F>::type;
        new(&data_) functor_type(std::forward<F>(f));
        using handle = fn_handler<functor_type, true, is_const, R, Args...>;
        using manage = fn_manager<functor_type, true, is_movable, is_copyable>;
        manage_ = &manage::manage;
        invoke_ = &handle::invoke;
    }

    // Construct dynamic.
    template<class F>
    void construct_(F&& f, std::false_type /*tag*/) {
        using functor_type = typename std::decay<F>::type;
        *(functor_type**)(&data_) = new functor_type(std::forward<F>(f));
        using handle = fn_handler<functor_type, false, is_const, R, Args...>;
        using manage = fn_manager<functor_type, false, is_movable, is_copyable>;
        manage_ = &manage::manage;
        invoke_ = &handle::invoke;
    }

    template<class F>
    void construct_(F&& f) {
        using functor_type = typename std::decay<F>::type;
        constexpr bool fit_local_storage =
            sizeof(functor_type) <= sizeof(storage_type);
        constexpr bool is_nothrow_movable =
            std::is_nothrow_move_constructible<functor_type>::value;
        constexpr bool use_local_storage =
            fit_local_storage && is_nothrow_movable;
        // Check dynamic allocation allowed.
        static_assert(
            !no_alloc || fit_local_storage, "insufficient storage size");
        static_assert(
            !no_alloc || is_nothrow_movable, " nothrow move required");

        construct_(
            std::forward<F>(f),
            std::integral_constant<bool, use_local_storage>());
    }

    void default_construct_() {
        invoke_ = &bad_call_;
        manage_ = nullptr;
    }

    void reset_() {
        if(manage_ == nullptr)
            return;
        manage_(exec_op::DESTRUCT, &data_, nullptr);
        default_construct_();
    }

    void copy_construct_(const function_data& rhs) {
        invoke_ = rhs.invoke_;
        manage_ = rhs.manage_;
        if(manage_ != nullptr)
            manage_(exec_op::COPY, rhs.get_data_(), &data_);
    }

    void copy_assign_(const function_data& rhs) {
        reset_();
        if(rhs.manage_ == nullptr)
            return;
        invoke_ = rhs.invoke_;
        manage_ = rhs.manage_;
        manage_(exec_op::COPY, rhs.get_data_(), &data_);
    }

    void move_construct_(function_data&& rhs) noexcept {
        invoke_ = rhs.invoke_;
        manage_ = rhs.manage_;
        if(manage_ == nullptr)
            return;
        rhs.manage_(exec_op::MOVE, &rhs.data_, &data_);
        rhs.default_construct_();
    }

    void move_assign_(function_data&& rhs) noexcept {
        reset_();
        if(rhs.manage_ == nullptr)
            return;
        rhs.manage_(exec_op::MOVE, &rhs.data_, &data_);
        invoke_ = rhs.invoke_;
        manage_ = rhs.manage_;
        rhs.default_construct_();
    }
};

// Const/mutable operator versions inheritance layer.
//============================================================================
template<class Enable, class Sig, size_t Size, fn_opt Options>
struct function_call_base;

// Common versions.
template<size_t Size, fn_opt Options, class R, class... Args>
struct function_call_base<
    enable_if_not_once<Options>, R(Args...), Size, Options>
    : function_data<Size, Options, false, R, Args...> {
    R operator()(Args... args) {
        return this->invoke_(this->get_data_(), static_cast<Args&&>(args)...);
    }
};

template<size_t Size, fn_opt Options, class R, class... Args>
struct function_call_base<
    enable_if_not_once<Options>, R(Args...) const, Size, Options>
    : function_data<Size, Options, true, R, Args...> {
    R operator()(Args... args) const {
        return this->invoke_(this->get_data_(), static_cast<Args&&>(args)...);
    }
};

// Call-once versions (both not const).
template<size_t Size, fn_opt Options, class R, class... Args>
struct function_call_base<enable_if_once<Options>, R(Args...), Size, Options>
    : function_data<Size, Options, false, R, Args...> {
    R operator()(Args... args) {
        auto moved_self = std::move(*this);
        return moved_self.invoke_(
            moved_self.get_data_(), static_cast<Args&&>(args)...);
    }
};

template<size_t Size, fn_opt Options, class R, class... Args>
struct function_call_base<
    enable_if_once<Options>, R(Args...) const, Size, Options>
    : function_data<Size, Options, true, R, Args...> {
    R operator()(Args... args) {
        auto moved_self = std::move(*this);
        return moved_self.invoke_(
            moved_self.get_data_(), static_cast<Args&&>(args)...);
    }
};

// Optional copy/move construction inheritance layer.
//============================================================================
template<class Enable, class Sig, size_t Size, fn_opt Options>
struct function_base;

template<class Sig, std::size_t Size, fn_opt Options>
struct function_base<
    enable_if_cpmove<Options, fn_opt::none>, Sig, Size, Options>
    : function_call_base<void, Sig, Size, Options> {
    constexpr function_base() noexcept = default;
    function_base(const function_base& /*rhs*/) = delete;
    function_base& operator=(const function_base& /*rhs*/) = delete;
    function_base(function_base&& /*rhs*/) = delete;
    function_base& operator=(function_base&& /*rhs*/) = delete;
};

template<class Sig, std::size_t Size, fn_opt Options>
struct function_base<
    enable_if_cpmove<Options, fn_opt::move>, Sig, Size, Options>
    : function_call_base<void, Sig, Size, Options> {
    constexpr function_base() noexcept = default;
    function_base(const function_base& /*rhs*/) = delete;
    function_base& operator=(const function_base& /*rhs*/) = delete;
    function_base(function_base&& rhs) noexcept {
        this->move_construct_(std::move(rhs));
    }
    function_base& operator=(function_base&& rhs) noexcept {
        this->move_assign_(std::move(rhs));
        return *this;
    }
};

template<class Sig, std::size_t Size, fn_opt Options>
struct function_base<
    enable_if_cpmove<Options, fn_opt::copy>, Sig, Size, Options>
    : function_call_base<void, Sig, Size, Options> {
    constexpr function_base() noexcept = default;
    function_base(const function_base& rhs) {
        this->copy_construct_(rhs);
    }
    function_base& operator=(const function_base& rhs) {
        this->copy_assign_(rhs);
        return *this;
    }
};

template<class Sig, std::size_t Size, fn_opt Options>
struct function_base<
    enable_if_cpmove<Options, fn_opt::copy_move>, Sig, Size, Options>
    : function_call_base<void, Sig, Size, Options> {
    constexpr function_base() noexcept = default;
    function_base(const function_base& rhs) {
        this->copy_construct_(rhs);
    }
    function_base& operator=(const function_base& rhs) {
        this->copy_assign_(rhs);
        return *this;
    }
    function_base(function_base&& rhs) noexcept {
        this->move_construct_(std::move(rhs));
    }
    function_base& operator=(function_base&& rhs) noexcept {
        this->move_assign_(std::move(rhs));
        return *this;
    }
};

template<class T>
struct is_function : std::false_type {};

template<class F, size_t Size, fn_opt Options>
struct is_function<basic_function<F, Size, Options>> : std::true_type {};

} // namespace function
} // namespace detail

// Basic function template.
//============================================================================
template<class Sig, size_t Size, fn_opt Options>
class basic_function
    : private detail::function::function_base<void, Sig, Size, Options> {
private:
    using base = detail::function::function_base<void, Sig, Size, Options>;

    // TODO(dsokolov): add const/noexcept checks
    template<class T>
    using accept_function = typename std::enable_if<
        !detail::function::is_function<typename std::decay<T>::type>::value,
        bool>::type;

public:
    constexpr basic_function() noexcept = default;
    constexpr basic_function(std::nullptr_t) noexcept {
    }

    basic_function& operator=(std::nullptr_t) {
        this->reset_();
        return *this;
    }

    template<class F, accept_function<F> = true>
    basic_function(F&& f) {
        this->construct_(std::forward<F>(f));
    }

    template<class F, accept_function<F> = true>
    basic_function& operator=(F&& f) {
        this->reset_();
        this->construct_(std::forward<F>(f));
        return *this;
    }

    template<class F, accept_function<F> = true>
    void assign(F&& f) {
        this->reset_();
        this->construct_(std::forward<F>(f));
    }

    void reset() {
        this->reset_();
    }

    using typename base::result_type;
    using base::operator();
    using base::operator bool;

    void swap(basic_function& rhs) noexcept {
        basic_function tmp = std::move(rhs);
        rhs = std::move(*this);
        *this = std::move(tmp);
    }
};

template<class F, std::size_t Size, fn_opt Options>
inline void swap(
    basic_function<F, Size, Options>& lhs,
    basic_function<F, Size, Options>& rhs) noexcept {
    lhs.swap(rhs);
}

template<class F, std::size_t Size, fn_opt Options>
inline bool operator==(
    std::nullptr_t, basic_function<F, Size, Options> const& f) {
    return !f;
}

template<class F, std::size_t Size, fn_opt Options>
inline bool operator==(
    basic_function<F, Size, Options> const& f, std::nullptr_t) {
    return !f;
}

template<class F, std::size_t Size, fn_opt Options>
inline bool operator!=(
    std::nullptr_t, basic_function<F, Size, Options> const& f) {
    return f;
}

template<class F, std::size_t Size, fn_opt Options>
inline bool operator!=(
    basic_function<F, Size, Options> const& f, std::nullptr_t) {
    return f;
}

// Common function types declaration.
//============================================================================
// Generic function.
template<class F, fn_opt Options = fn_opt::copy_move>
using function = basic_function<F, detail::function::default_size, Options>;

// Function with adjustable preallocated size.
template<class F, size_t Size, fn_opt Options = fn_opt::copy_move>
using so_function = basic_function<F, Size, Options>;

// Fixed size function, dynamic allocation forbidden, no copy-move constructors.
template<class F, size_t Size, fn_opt Options = fn_opt::none>
using fs_function = basic_function<F, Size, Options | fn_opt::no_alloc>;

} // namespace univang

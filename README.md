This project aims to provide a support for debugging stackfull coroutines
from [boost.Coroutine2](https://www.boost.org/doc/libs/1_81_0/libs/coroutine2/doc/html/index.html). <br>
An **ll**db plugin for boost.**C**oroutine**2**, hence the name **llc2**.

The plugin is very limited in functionality and is focused exclusively
on [uServer](https://github.com/userver-framework/userver), <br>
and although it may work for other use cases this isn't a priority for now.

### Quick start

Start an LLDB session, load the plugin, configure it and backtrace sleeping coroutines.

```
(lldb) plugin load /home/itrofimow/work/llc2/build/libllc2.so
(lldb) llc2 init -s 262144 -c fcontext
LLC2 plugin initialized. Settings:
stack_size: 262144
context implementation: fcontext
with magic: false
filter by: (null)
truncate at: (null)
(lldb) llc2 bt
----------------------------------------------------------------------------------------- FOUND SLEEPING COROUTINE -----------------------------------------------------------------------------------------
coro stack address: 0x7fffecf2e000
----------------------------------
Current span (name, span_id, trace_id): http/handler-hello-sample | 75e2fb6fe8dff5c1 | 66acd8c970ec4923879c17396bba7a5c
-----------------------------------------------------------------------------------------------------------------------
frame #0: 0x00000000003025f9 userver-samples-hello_service`boost::coroutines2::detail::pull_coroutine<userver::engine::impl::TaskContext*>::control_block::resume() [inlined] std::enable_if<__and_<std::__not_<std::__is_tuple_like<void*> >, std::is_move_constructible<void*>, std::is_move_assignable<void*> >::value, void>::type std::swap<void*>(__a=0x00007fffffffd210, __b=<unavailable>) at move.h:204:19
frame #1: 0x00000000003025f9 userver-samples-hello_service`boost::coroutines2::detail::pull_coroutine<userver::engine::impl::TaskContext*>::control_block::resume() [inlined] boost::context::fiber::swap(this=0x00007fffffffd210, other=<unavailable>) at fiber_fcontext.hpp:335
frame #2: 0x00000000003025f9 userver-samples-hello_service`boost::coroutines2::detail::pull_coroutine<userver::engine::impl::TaskContext*>::control_block::resume() at fiber_fcontext.hpp:269
frame #3: 0x00000000003025f9 userver-samples-hello_service`boost::coroutines2::detail::pull_coroutine<userver::engine::impl::TaskContext*>::control_block::resume(this=0x00007fffffffd210) at pull_control_block_cc.ipp:147
frame #4: 0x00000000002fea45 userver-samples-hello_service`userver::engine::impl::TaskContext::Sleep(userver::engine::impl::WaitStrategy&) [inlined] boost::coroutines2::detail::pull_coroutine<userver::engine::impl::TaskContext*>::operator(this=<unavailable>)() at pull_coroutine.ipp:77:10
frame #5: 0x00000000002fea3d userver-samples-hello_service`userver::engine::impl::TaskContext::Sleep(this=0x00007fffde2dd800, wait_strategy=0x00007fffffffd2f0) at task_context.cpp:331
frame #6: 0x000000000029963d userver-samples-hello_service`samples::hello::Hello::HandleRequestThrow[abi:cxx11](userver::server::http::HttpRequest const&, userver::server::request::RequestContext&) const [inlined] userver::engine::InterruptibleSleepUntil(deadline=<unavailable>) at sleep.cpp:28:11
frame #7: 0x000000000029960c userver-samples-hello_service`samples::hello::Hello::HandleRequestThrow[abi:cxx11](userver::server::http::HttpRequest const&, userver::server::request::RequestContext&) const [inlined] userver::engine::SleepUntil(deadline=<unavailable>) at sleep.cpp:33
frame #8: 0x0000000000299604 userver-samples-hello_service`samples::hello::Hello::HandleRequestThrow[abi:cxx11](userver::server::http::HttpRequest const&, userver::server::request::RequestContext&) const [inlined] void userver::engine::SleepFor<long, std::ratio<1l, 1l> >(duration=<unavailable>) at sleep.hpp:44
frame #9: 0x00000000002995eb userver-samples-hello_service`samples::hello::Hello::HandleRequestThrow[abi:cxx11](this=<unavailable>, (null)=<unavailable>, (null)=<unavailable>) const at hello_service.cpp:22
frame #10: 0x000000000034a906 userver-samples-hello_service`userver::server::handlers::HttpHandlerBase::HandleRequest(userver::server::request::RequestBase&, userver::server::request::RequestContext&) const [inlined] userver::server::handlers::HttpHandlerBase::HandleRequest(this=<unavailable>) const::$_19::operator()() const at http_handler_base.cpp:574:30
frame #11: 0x000000000034a7c3 userver-samples-hello_service`userver::server::handlers::HttpHandlerBase::HandleRequest(userver::server::request::RequestBase&, userver::server::request::RequestContext&) const [inlined] bool userver::server::handlers::(anonymous namespace)::RequestProcessor::DoProcessRequestStep<userver::server::handlers::HttpHandlerBase::HandleRequest(this=0x00007fffecf6ca98, step_name=<unavailable>, process_step_func=<unavailable>) const::$_19>(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, userver::server::handlers::HttpHandlerBase::HandleRequest(userver::server::request::RequestBase&, userver::server::request::RequestContext&) const::$_19 const&) at http_handler_base.cpp:171
frame #12: 0x000000000034a6e1 userver-samples-hello_service`userver::server::handlers::HttpHandlerBase::HandleRequest(userver::server::request::RequestBase&, userver::server::request::RequestContext&) const [inlined] bool userver::server::handlers::(anonymous namespace)::RequestProcessor::ProcessRequestStep<userver::server::handlers::HttpHandlerBase::HandleRequest(this=0x00007fffecf6ca98, step_name=<unavailable>, process_step_func=<unavailable>) const::$_19>(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, userver::server::handlers::HttpHandlerBase::HandleRequest(userver::server::request::RequestBase&, userver::server::request::RequestContext&) const::$_19 const&) at http_handler_base.cpp:159
frame #13: 0x000000000034a6cb userver-samples-hello_service`userver::server::handlers::HttpHandlerBase::HandleRequest(this=0x00007fffdcc27080, request=0x00007fffdc88e410, context=0x00007fffecf6dcf0) const at http_handler_base.cpp:568
----------------------------------------------------------------------------------------- FOUND SLEEPING COROUTINE -----------------------------------------------------------------------------------------
coro stack address: 0x7fffed37f000
... coro frames follow ...
```

### Build linux

Install lldb development headers (liblldb-XX-dev) of matching lldb version, <br>
next, clone this code and run <br>
`mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make` <br>
Now you have a `libllc2.so` and should be able to `plugin load` it into lldb.

### Build macos

Install llvm package using brew `brew install llvm`, <br>
next, clone this code and run <br>
`mkdir build && cd build && cmake -DLLVM_DIR=/usr/local/opt/llvm/lib/cmake/llvm -DCMAKE_BUILD_TYPE=Release .. && make` <br>
Now you have a `libllc2.dylib` and should be able to `plugin load` at into lldb.

### Limitations

* x86_64 linux and macos
* Only works
  with [protected_fixedsize](https://www.boost.org/doc/libs/1_81_0/libs/coroutine2/doc/html/coroutine2/stack/protected_fixedsize.html)
* Some of `llc2 init` options are completely ignored: `-f`, `-t`. They are hardcoded to uServer-specific values for now.
* Might not work with different versions of boost.Coroutine2
* Might accidentally not work at all

### llc2 init

Supported options:

* `-s` - stack size of a coroutine. For `uServer` it should be either default value of 256Kb or that specified
  in `static_config.yaml`
* `-c` - context implementation, either `ucontext` or `fcontext`. For `uServer` it should be `fcontext`, until the
  binary is built with sanitizers, then `ucontext`.

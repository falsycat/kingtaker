#include "kingtaker.hh"

#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>

#include <lua.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <ImNodes.h>

#include "iface/node.hh"

#include "util/gui.hh"
#include "util/luajit.hh"
#include "util/node.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"


namespace kingtaker {
namespace {

luajit::Device dev_;


class Compile final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Compile>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "LuaJIT/Compile", "compile string into Lua object",
      {typeid(iface::Node)});

  static inline const std::vector<SockMeta> kInSocks = {
    { .name = "clear", .type = SockMeta::kPulse, .trigger = true, },
    { .name = "name",  .type = SockMeta::kString, .def = ""s, },
    { .name = "src",   .type = SockMeta::kStringMultiline, },
    { .name = "exec",  .type = SockMeta::kPulse, .trigger = true, },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { .name = "out", .type = SockMeta::kPulse, },
  };

  Compile(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  std::string title() const noexcept {
    return "LuaJIT Compile";
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:
      Clear();
      return;
    case 1:
      name_ = v.string();
      return;
    case 2:
      src_ = v.stringPtr();
      return;
    case 3:
      Exec();
      return;
    }
    assert(false);
  }
  void Clear() noexcept {
    name_ = "";
    src_  = nullptr;
  }
  void Exec() {
    auto  ctx   = ctx_.lock();
    auto& out   = owner_->sharedOut(0);
    auto& error = owner_->sharedOut(1);

    auto task = [owner = owner_, ctx, name = name_, src = src_, out, error](auto L) {
      static const auto lua_reader = [](auto, void* data, size_t* size) -> const char* {
        auto& ptr = *reinterpret_cast<const std::string**>(data);
        if (ptr) {
          *size = ptr->size();
          auto ret = ptr->c_str();
          ptr = nullptr;
          return ret;
        }
        return nullptr;
      };
      const std::string* ptr = src.get();
      if (lua_load(L, lua_reader, (void*) &ptr, name.c_str()) == 0) {
        auto obj = luajit::Obj::PopAndCreate(&dev_, L);
        out->Send(ctx, std::static_pointer_cast<Value::Data>(obj));
      } else {
        ctx->Notify(owner, lua_tostring(L, -1));
        error->Send(ctx, {});
      }
    };
    dev_.Queue(std::move(task));
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;

  std::string name_;

  std::shared_ptr<const std::string> src_;
};


class Exec final : public File, public iface::Node {
 public:
  static inline TypeInfo kType = TypeInfo::New<Exec>(
      "LuaJIT/Exec", "execute compiled function",
      {typeid(iface::Node)});

  static inline const SockMeta kOutRecv = {
    .name = "recv", .type = SockMeta::kPulse,
  };
  static inline const SockMeta kInFunc = {
    .name = "func", .type = SockMeta::kData, .dataType = luajit::Obj::kName,
  };
  static inline const SockMeta kInSend = {
    .name = "send", .type = SockMeta::kAny, .trigger = true,
  };

  Exec(Env* env) noexcept :
      File(&kType, env), Node(Node::kNone),
      sock_recv_(std::make_shared<OutSock>(this, &kOutRecv)) {
    auto task_func = [this](auto& ctx, auto&& v) {
      try {
        auto cdata = ctx->template data<ContextData>(this, this, ctx);
        cdata->func = v.template dataPtr<luajit::Obj>();
      } catch (Exception& e) {
        ctx->Notify(this, e.msg());
      }
    };
    sock_func_.emplace(this, &kInFunc, std::move(task_func));

    auto task_send = [this](auto& ctx, auto&& v) {
      try {
        Send(ctx->template data<ContextData>(this, this, ctx), std::move(v));
      } catch (Exception& e) {
        ctx->Notify(this, e.msg());
      }
    };
    sock_send_.emplace(this, &kInSend, std::move(task_send));

    out_ = {sock_recv_.get()};
    in_  = {&*sock_func_, &*sock_send_};
  }

  Exec(Env* env, const msgpack::object&) noexcept :
      Exec(env) {
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack_nil();
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<Exec>(env);
  }

  void UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  std::shared_ptr<OutSock> sock_recv_;
  std::optional<NodeLambdaInSock> sock_func_;
  std::optional<NodeLambdaInSock> sock_send_;


  class ContextData final : public Context::Data {
   public:
    static void Push(lua_State* L, const std::shared_ptr<ContextData>& cdata) {
      dev_.NewObjWithoutMeta<std::weak_ptr<ContextData>>(L, cdata);
      if (luaL_newmetatable(L, "Exec_ContextData")) {
        lua_createtable(L, 0, 0);
          lua_pushcfunction(L, L_notify);
          lua_setfield(L, -2, "notify");

          lua_pushcfunction(L, L_emit);
          lua_setfield(L, -2, "emit");

          lua_pushcfunction(L, L_table);
          lua_setfield(L, -2, "table");
        lua_setfield(L, -2, "__index");

        dev_.PushObjDeleter<std::weak_ptr<ContextData>>(L);
        lua_setfield(L, -2, "__gc");
      }
      lua_setmetatable(L, -2);
    }
    static std::shared_ptr<ContextData> Get(lua_State* L, int idx) {
      auto wcdata = dev_.GetObj<std::weak_ptr<ContextData>>(L, idx, "Exec_ContextData");
      auto cdata  = wcdata->lock();
      if (!cdata) luaL_error(L, "context data is expired");
      return cdata;
    }

    ContextData(Exec* o, const std::weak_ptr<Context>& ctx) noexcept :
        owner_(o), ctx_(ctx) {
    }

    void Clear(lua_State* L) noexcept {
      luaL_unref(L, LUA_REGISTRYINDEX, reg_table_);
      reg_table_ = LUA_REFNIL;
    }

    Exec* owner() const noexcept { return owner_; }

    std::shared_ptr<Context> ctx() const {
      auto ret = ctx_.lock();
      if (!ret) throw Exception("context is expired");
      return ret;
    }
    std::shared_ptr<OutSock> out() const {
      auto ret = out_.lock();
      if (!ret) throw Exception("socket is expired");
      return ret;
    }

    std::shared_ptr<luajit::Obj> func;

   private:
    Exec* owner_;

    std::weak_ptr<Context> ctx_;
    std::weak_ptr<OutSock> out_;

    int reg_table_ = LUA_REFNIL;


    static int L_notify(lua_State* L) {
      try {
        auto cdata = Get(L, 1);
        auto ctx   = cdata->ctx();

        auto msg = luaL_checkstring(L, 2);
        ctx->Notify(cdata->owner_, msg);
        return 0;
      } catch (Exception& e) {
        return luaL_error(L, e.msg().c_str());
      }
    }
    static int L_emit(lua_State* L) {
      try {
        auto cdata = Get(L, 1);
        auto ctx   = cdata->ctx();
        auto out   = cdata->out();

        const auto& v = *dev_.GetObj<Value>(L, 2, "Value");
        out->Send(ctx, Value(v));
        return 0;
      } catch (Exception& e) {
        return luaL_error(L, e.msg().c_str());
      }
    }
    static int L_table(lua_State* L) {
      try {
        auto cdata = Get(L, 1);
        if (cdata->reg_table_ == LUA_REFNIL) {
          lua_createtable(L, 0, 0);
          lua_pushvalue(L, -1);
          cdata->reg_table_ = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
          lua_rawgeti(L, LUA_REGISTRYINDEX, cdata->reg_table_);
        }
        return 1;
      } catch (Exception& e) {
        return luaL_error(L, e.msg().c_str());
      }
    }
  };


  static void Send(std::shared_ptr<ContextData> cdata, Value&& v) {
    auto func = cdata->func;
    if (!func) throw Exception("func is not specified");

    auto ctx  = cdata->ctx();
    auto task = [ctx, cdata, func, v = std::move(v)](auto L) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, func->reg());
      dev_.PushValue(L, v);
      ContextData::Push(L, cdata);
      if (dev_.SandboxCall(L, 2, 0) != 0) {
        ctx->Notify(cdata->owner(), lua_tostring(L, -1));
      }
    };
    dev_.Queue(std::move(task));
  }
};
void Exec::UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept {
  ImGui::TextUnformatted("LuaJIT Exec");

  ImGui::BeginGroup();
  gui::NodeInSock(kInFunc);
  gui::NodeInSock(kInSend);
  ImGui::EndGroup();

  ImGui::SameLine();

  ImGui::BeginGroup();
  gui::NodeOutSock(kOutRecv);
  ImGui::EndGroup();
}

} }  // namespace kingtaker

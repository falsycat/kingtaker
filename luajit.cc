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
#include "util/notify.hh"
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
    { "clear", "", kPulseButton },
    { "name",  "", },
    { "src",   "", },
    { "exec",  "", kPulseButton | kExecIn },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "out",   "", },
    { "error", "", },
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
    auto ctx   = ctx_.lock();
    auto out   = owner_->out(0);
    auto error = owner_->out(1);

    auto task = [owner = owner_, path = owner_->path(),
                 ctx, name = name_, src = src_, out, error](auto L) {
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
        notify::Error(path, owner, lua_tostring(L, -1));
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

  Exec(Env* env) noexcept :
      File(&kType, env), Node(Node::kNone),
      udata_(std::make_shared<UniversalData>()) {
    out_.emplace_back(new OutSock(this, "recv"));
    out_.emplace_back(new OutSock(this, "abort"));

    udata_->self      = this;
    udata_->out_recv  = out_[0];
    udata_->out_abort = out_[1];

    auto task_clear = [udata = udata_](auto& ctx, auto&&) {
      auto cdata = ContextData::Get(udata, ctx);
      dev_.Queue([cdata](auto L) { cdata->Clear(L); });
    };
    in_.emplace_back(new NodeLambdaInSock(this, "clear", std::move(task_clear)));

    auto task_func = [udata = udata_](auto& ctx, auto&& v) {
      try {
        auto cdata = ContextData::Get(udata, ctx);
        cdata->func = v.template dataPtr<luajit::Obj>();
      } catch (Exception& e) {
        notify::Warn(udata->pathSync(), udata->self, e.msg());
      }
    };
    in_.emplace_back(new NodeLambdaInSock(this, "func", std::move(task_func)));

    auto task_send = [udata = udata_](auto& ctx, auto&& v) {
      try {
        Send(ContextData::Get(udata, ctx), std::move(v));
      } catch (Exception& e) {
        notify::Error(udata->pathSync(), udata->self, e.msg());
      }
    };
    in_.emplace_back(new NodeLambdaInSock(this, "send", std::move(task_send)));
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

  void Update(RefStack& ref, Event&) noexcept override {
    std::unique_lock<std::mutex> k(udata_->mtx);
    udata_->path = ref.GetFullPath();
  }
  void UpdateNode(RefStack&, const std::shared_ptr<Editor>&) noexcept override;

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::Node>(t).Select(this);
  }

 private:
  class UniversalData final {
   public:
    std::mutex mtx;

    Path path;
    Path pathSync() const noexcept {
      std::unique_lock<std::mutex> k(const_cast<std::mutex&>(mtx));
      return Path(path);
    }

    // immutable
    Exec* self;

    std::shared_ptr<OutSock> out_recv;
    std::shared_ptr<OutSock> out_abort;
  };
  std::shared_ptr<UniversalData> udata_;


  class ContextData final : public Context::Data {
   public:
    static std::shared_ptr<ContextData> Get(
        const std::shared_ptr<UniversalData>& udata,
        const std::shared_ptr<Context>&       ctx) noexcept {
      return ctx->data<ContextData>(udata->self, udata, ctx);
    }
    static void Push(lua_State* L, const std::shared_ptr<ContextData>& cdata) {
      dev_.NewObjWithoutMeta<std::weak_ptr<ContextData>>(L, cdata);
      if (luaL_newmetatable(L, "Exec_ContextData")) {
        lua_createtable(L, 0, 0);
          lua_pushcfunction(L, L_log<notify::kTrace>);
          lua_setfield(L, -2, "trace");

          lua_pushcfunction(L, L_log<notify::kInfo>);
          lua_setfield(L, -2, "info");

          lua_pushcfunction(L, L_log<notify::kWarn>);
          lua_setfield(L, -2, "warn");

          lua_pushcfunction(L, L_log<notify::kError>);
          lua_setfield(L, -2, "error");

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

    ContextData(const std::weak_ptr<UniversalData>& udata,
                const std::weak_ptr<Context>&       ctx) noexcept :
        udata_(udata), ctx_(ctx) {
    }

    void Clear(lua_State* L) noexcept {
      luaL_unref(L, LUA_REGISTRYINDEX, reg_table_);
      reg_table_ = LUA_REFNIL;
    }

    std::shared_ptr<UniversalData> udata() const {
      auto ret = udata_.lock();
      if (!ret) throw Exception("universal data is expired");
      return ret;
    }
    std::shared_ptr<Context> ctx() const {
      auto ret = ctx_.lock();
      if (!ret) throw Exception("context is expired");
      return ret;
    }

    std::shared_ptr<luajit::Obj> func;

   private:
    std::weak_ptr<UniversalData> udata_;

    std::weak_ptr<Context> ctx_;

    int reg_table_ = LUA_REFNIL;


    template <notify::Level kLv>
    static int L_log(lua_State* L) {
      try {
        auto cdata = Get(L, 1);
        auto udata = cdata->udata();

        auto msg = luaL_checkstring(L, 2);
        notify::Push({std::source_location::current(),
                     kLv, msg, Path(udata->pathSync()), udata->self});
        return 0;
      } catch (Exception& e) {
        return luaL_error(L, e.msg().c_str());
      }
    }
    static int L_emit(lua_State* L) {
      try {
        auto cdata = Get(L, 1);
        auto udata = cdata->udata();
        auto ctx   = cdata->ctx();

        const auto& v = *dev_.GetObj<Value>(L, 2, "Value");
        udata->out_recv->Send(ctx, Value(v));
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

    auto udata = cdata->udata();
    auto ctx   = cdata->ctx();
    auto task = [udata, ctx, cdata, func, v = std::move(v)](auto L) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, func->reg());
      dev_.PushValue(L, v);
      ContextData::Push(L, cdata);
      if (dev_.SandboxCall(L, 2, 0) != 0) {
        notify::Error(udata->path, udata->self, lua_tostring(L, -1));
        udata->out_abort->Send(ctx, {});
      }
    };
    dev_.Queue(std::move(task));
  }
};
void Exec::UpdateNode(RefStack&, const std::shared_ptr<Editor>& ctx) noexcept {
  ImGui::TextUnformatted("LuaJIT Exec");

  ImGui::BeginGroup();
  if (ImNodes::BeginInputSlot("clear", 1)) {
    gui::NodeInSock(ctx, in_[0], true  /* = small */);
    ImNodes::EndSlot();
  }
  if (ImNodes::BeginInputSlot("func", 1)) {
    gui::NodeInSock("func");
    ImNodes::EndSlot();
  }
  if (ImNodes::BeginInputSlot("send", 1)) {
    gui::NodeInSock("send");
    ImNodes::EndSlot();
  }
  ImGui::EndGroup();

  ImGui::SameLine();

  ImGui::BeginGroup();
  const auto r = ImGui::GetCursorPosX() + 4*ImGui::GetFontSize();
  ImGui::SetCursorPosX(r - ImGui::CalcTextSize("recv").x);
  if (ImNodes::BeginOutputSlot("recv", 1)) {
    gui::NodeOutSock("recv");
    ImNodes::EndSlot();
  }
  ImGui::SetCursorPosX(r - ImGui::CalcTextSize("abort").x);
  if (ImNodes::BeginOutputSlot("abort", 1)) {
    gui::NodeOutSock("abort");
    ImNodes::EndSlot();
  }
  ImGui::EndGroup();
}

} }  // namespace kingtaker

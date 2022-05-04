#include <cassert>
#include <cinttypes>
#include <string>
#include <variant>

#include <imgui.h>
#include <imgui_internal.h>

#include "kingtaker.hh"

#include "iface/dir.hh"
#include "iface/node.hh"

#include "util/gl.hh"
#include "util/gui.hh"
#include "util/node.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"

namespace kingtaker {

static void GetResolution(const Value& v, int32_t& w, int32_t& h) {
  constexpr int32_t kMaxReso = 4096;

  const auto size = v.tuple().float2();
  w = static_cast<int32_t>(size.x);
  h = static_cast<int32_t>(size.y);
  if (w <= 0 || h <= 0 || w > kMaxReso || h > kMaxReso) {
    throw Exception("out of range");
  }
}


class Texture final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Texture>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "GL/Texture", "A node that creates renderbuffer object",
      {typeid(iface::Node)});

  static std::string title() noexcept { return "GL Texture"; }

  static inline const std::vector<SockMeta> kInSocks = {
    { .name = "clear",  .type = SockMeta::kPulse, .trigger = true, },
    { .name = "reso",   .type = SockMeta::kVec2, },
    { .name = "format", .type = SockMeta::kStringOption,
      .stringOptions = gl::GetEnumNames(gl::kFormats), },
    { .name = "exec",   .type = SockMeta::kPulse, .trigger = true, },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { .name = "out", .type = SockMeta::kData, },
  };

  Texture() = delete;
  Texture(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:
      Clear();
      return;
    case 1:
      GetResolution(v, w_, h_);
      return;
    case 2:
      format_ = gl::ParseFormat<Exception>(v.string()).gl;
      return;
    case 3:
      Exec();
      return;
    }
    assert(false);
  }
  void Clear() {
    w_ = 0, h_ = 0;
    format_ = 0;
  }
  void Exec() {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    auto& out = owner_->sharedOut(0);

    if (w_ == 0 || h_ == 0) {
      throw Exception("resolution is unspecified");
    }
    if (format_ == 0) {
      throw Exception("format is unspecified");
    }

    auto tex = gl::Texture::Create(GL_TEXTURE_2D);
    // TODO set metadata
    auto task = [fmt = format_, w = w_, h = h_, tex, ctx, out]() {
      const bool depth =
          fmt == GL_DEPTH_COMPONENT ||
          fmt == GL_DEPTH_COMPONENT16 ||
          fmt == GL_DEPTH_COMPONENT24 ||
          fmt == GL_DEPTH_COMPONENT32F;
      const GLenum exfmt = depth? GL_DEPTH_COMPONENT: GL_RED;

      glBindTexture(GL_TEXTURE_2D, tex->id());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D,
                   0, static_cast<GLint>(fmt), w, h, 0,
                   exfmt, GL_UNSIGNED_BYTE, nullptr);
      glBindTexture(GL_TEXTURE_2D, 0);

      assert(glGetError() == GL_NO_ERROR);
      out->Send(ctx, std::dynamic_pointer_cast<Value::Data>(tex));
    };
    Queue::gl().Push(std::move(task));
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;

  int32_t w_ = 0, h_ = 0;

  GLenum format_ = 0;
};


class Renderbuffer final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Renderbuffer>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "GL/Renderbuffer", "A node that creates renderbuffer object",
      {typeid(iface::Node)});

  static std::string title() noexcept { return "GL Renderbuffer"; }

  static inline const std::vector<SockMeta> kInSocks = {
    { .name = "clear",   .type = SockMeta::kPulse, .trigger = true, },
    { .name = "reso",    .type = SockMeta::kVec2, },
    { .name = "format",  .type = SockMeta::kStringOption,
      .stringOptions = gl::GetEnumNames(gl::kFormats), },
    { .name = "samples", .type = SockMeta::kInteger, },
    { .name = "exec",    .type = SockMeta::kPulse, .trigger = true, },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { .name = "out", .type = SockMeta::kData, },
  };

  Renderbuffer() = delete;
  Renderbuffer(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:
      Clear();
      return;
    case 1:
      GetResolution(v, w_, h_);
      return;
    case 2:
      format_ = gl::ParseFormat<Exception>(v.string()).gl;
      return;
    case 3:
      samples_ = v.integer<GLsizei>();
      return;
    case 4:
      Exec();
      return;
    }
    assert(false);
  }
  void Clear() {
    w_ = 0, h_ = 0;
    format_  = 0;
    samples_ = 0;
  }
  void Exec() {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    auto& out = owner_->sharedOut(0);

    if (w_ == 0 || h_ == 0) {
      throw Exception("resolution is unspecified");
    }
    if (format_ == 0) {
      throw Exception("format is unspecified");
    }

    auto rb = gl::Renderbuffer::Create(GL_RENDERBUFFER);
    // TODO set metadata
    auto task = [samples = samples_, fmt = format_, w = w_, h = h_, rb, ctx, out]() {
      glBindRenderbuffer(GL_RENDERBUFFER, rb->id());
      glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, fmt, w, h);
      glBindRenderbuffer(GL_RENDERBUFFER, 0);

      assert(glGetError() == GL_NO_ERROR);
      out->Send(ctx, std::dynamic_pointer_cast<Value::Data>(rb));
    };
    Queue::gl().Push(std::move(task));
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;

  int32_t w_ = 0, h_ = 0;
  GLenum format_ = 0;
  GLsizei samples_ = 0;
};


class Framebuffer final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Framebuffer>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "GL/Framebuffer", "A node that creates framebuffer object",
      {typeid(iface::Node)});

  static std::string title() noexcept { return "GL Framebuffer"; }

  static inline const std::vector<SockMeta> kInSocks = {
    { .name = "clear",  .type = SockMeta::kPulse, .trigger = true, },
    { .name = "reso",   .type = SockMeta::kVec2,  },
    { .name = "attach", .type = SockMeta::kNamedValue, },
    { .name = "exec",   .type = SockMeta::kPulse, .trigger = true,  },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { .name = "out",   .type = SockMeta::kData, },
  };

  Framebuffer() = delete;
  Framebuffer(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:
      Clear();
      return;
    case 1:
      GetResolution(v, w_, h_);
      return;
    case 2:
      Attach(std::move(v));
      return;
    case 3:
      Exec();
      return;
    }
  }
  void Clear() noexcept {
    w_ = 0, h_ = 0;
    fb_ = nullptr;
  }
  void Attach(Value&& v) {
    if (!fb_) fb_ = gl::Framebuffer::Create(GL_FRAMEBUFFER);

    const auto& tup = v.tuple();

    const auto& at    = gl::ParseAttachment<Exception>(tup[0].string());
    const auto& data  = v.tuple()[1].dataPtr();

    // validate tex or rb
    auto tex = std::dynamic_pointer_cast<gl::Texture>(data);
    auto rb  = std::dynamic_pointer_cast<gl::Renderbuffer>(data);
    if (!tex && !rb) {
      throw Exception(at.name+" is not pulse, gl::Texture, or gl::Renderbuffer");
    }
    if (tex && tex->gl() != GL_TEXTURE_2D) {
      throw Exception(at.name+" is gl::Texture, but not GL_TEXTURE_2D");
    }

    // TODO tex or rb size validation
    // TODO tex or rb type validation

    // attach to fb
    auto task = [&at, fb = fb_, tex, rb]() {
      glBindFramebuffer(GL_FRAMEBUFFER, fb->id());
      if (tex) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, at.gl, GL_TEXTURE_2D, tex->id(), 0);
      } else if (rb) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, at.gl, GL_RENDERBUFFER, rb->id());
      }
      glBindFramebuffer(GL_FRAMEBUFFER, 0);

      assert(glGetError() == GL_NO_ERROR);
    };
    Queue::gl().Push(std::move(task));
  }
  void Exec() {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    if (w_ == 0 || h_ == 0) {
      throw Exception("resolution is unspecified");
    }
    if (!fb_) {
      throw Exception("attach something firstly");
    }

    auto& out = owner_->sharedOut(0);

    // check status and emit result
    auto task = [owner = owner_, fb = fb_, ctx, out]() {
      glBindFramebuffer(GL_FRAMEBUFFER, fb->id());
      const auto stat = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (stat == GL_FRAMEBUFFER_COMPLETE) {
        out->Send(ctx, std::dynamic_pointer_cast<Value::Data>(fb));
      } else {
        ctx->Notify(owner, "broken framebuffer ("+std::to_string(stat)+")");
      }
      glBindFramebuffer(GL_FRAMEBUFFER, 0);

      assert(glGetError() == GL_NO_ERROR);
    };
    Queue::gl().Push(std::move(task));

    // drop fb for next creation
    fb_ = nullptr;
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;

  std::shared_ptr<gl::Framebuffer> fb_;

  int32_t w_ = 0, h_ = 0;
};


class VertexArray final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<VertexArray>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "GL/VertexArray", "A node that creates vertex array object",
      {typeid(iface::Node)});

  static std::string title() noexcept { return "GL VAO"; }

  static inline const std::vector<SockMeta> kInSocks = {
    { .name = "exec", .type = SockMeta::kPulse, .trigger = true, },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { .name = "out", .type = SockMeta::kData, },
  };

  VertexArray() = delete;
  VertexArray(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&&) {
    switch (idx) {
    case 0: Exec(); return;
    }
    assert(false);
  }
  void Exec() {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    auto& out = owner_->sharedOut(0);

    auto vao  = gl::VertexArray::Create(0);
    auto task = [vao, ctx, out]() {
      out->Send(ctx, std::dynamic_pointer_cast<Value::Data>(vao));
    };
    Queue::gl().Push(std::move(task));
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;
};


class Program final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Program>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "GL/Program", "A node that links program object",
      {typeid(iface::Node)});

  static std::string title() noexcept { return "GL Program"; }

  static inline const std::vector<SockMeta> kInSocks = {
    { .name = "clear",   .type = SockMeta::kPulse, .trigger = true, },
    { .name = "shaders", .type = SockMeta::kData, .multi = true, },
    { .name = "exec",    .type = SockMeta::kPulse, .trigger = true,  },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { .name = "out", .type = SockMeta::kData, },
  };

  Program() = delete;
  Program(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:
      Clear();
      return;
    case 1:
      AttachShader(std::move(v));
      return;
    case 2:
      Exec();
      return;
    }
    assert(false);
  }
  void Clear() noexcept {
    prog_ = nullptr;
  }
  void AttachShader(Value&& v) {
    if (!prog_) prog_ = gl::Program::Create(0);

    const auto shader = v.dataPtr<gl::Shader>();

    auto task = [prog = prog_, shader]() {
      glAttachShader(prog->id(), shader->id());
      assert(glGetError() == GL_NO_ERROR);
    };
    Queue::gl().Push(std::move(task));
  }
  void Exec() {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    if (!prog_) throw Exception("attach shaders firstly");

    auto out = owner_->sharedOut(0);

    // link program and check status
    auto task = [owner = owner_, prog = prog_, ctx, out]() {
      const auto id = prog->id();
      glLinkProgram(id);

      GLint linked;
      glGetProgramiv(id, GL_LINK_STATUS, &linked);
      if (linked == GL_TRUE) {
        out->Send(ctx, std::dynamic_pointer_cast<Value::Data>(prog));
      } else {
        GLsizei len = 0;
        char buf[1024];
        glGetProgramInfoLog(id, sizeof(buf), &len, buf);
        ctx->Notify(owner, buf);
      }
      assert(glGetError() == GL_NO_ERROR);
    };
    Queue::gl().Push(std::move(task));

    prog_ = nullptr;
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;

  std::shared_ptr<gl::Program> prog_;
};


class Shader final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<Shader>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "GL/Shader", "A node that compiles shader",
      {typeid(iface::Node)});

  static std::string title() noexcept { return "GL Shader"; }

  static inline const std::vector<SockMeta> kInSocks = {
    { .name = "clear", .type = SockMeta::kPulse, .trigger = true, },
    { .name = "type",  .type = SockMeta::kStringOption,
      .stringOptions = gl::GetEnumNames(gl::kShaderTypes), },
    { .name = "src",   .type = SockMeta::kStringMultiline, .multi = true, },
    { .name = "exec",  .type = SockMeta::kPulse, .trigger = true, },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { .name = "out",   .type = SockMeta::kData, },
    { .name = "error", .type = SockMeta::kPulse, },
  };

  Shader() = delete;
  Shader(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:
      Clear();
      return;
    case 1:
      t_ = gl::ParseShaderType(v.string()).gl;
      return;
    case 2:
      srcs_.push_back(v.stringPtr());
      return;
    case 3:
      Exec();
      return;
    }
    assert(false);
  }
  void Clear() {
    t_ = 0;
    srcs_.clear();
  }
  void Exec() {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    if (t_ == 0) {
      throw Exception("type is unspecified");
    }
    if (srcs_.empty()) {
      throw Exception("src is unspecified");
    }

    auto& out   = owner_->sharedOut(0);
    auto& error = owner_->sharedOut(1);

    auto shader = gl::Shader::Create(t_);
    auto task = [owner = owner_, shader, srcs = srcs_, ctx, out, error]() {
      const auto id = shader->id();

      std::vector<GLchar*> ptrs;
      ptrs.reserve(srcs.size());
      for (const auto& src : srcs) {
        ptrs.push_back(const_cast<GLchar*>(src->c_str()));
      }
      glShaderSource(id, static_cast<GLsizei>(srcs.size()), &ptrs[0], nullptr);
      glCompileShader(id);

      GLint compiled;
      glGetShaderiv(id, GL_COMPILE_STATUS, &compiled);
      if (compiled == GL_TRUE) {
        out->Send(ctx, std::dynamic_pointer_cast<Value::Data>(shader));
      } else {
        GLsizei len = 0;
        char buf[1024];
        glGetShaderInfoLog(id, sizeof(buf), &len, buf);
        ctx->Notify(owner, buf);
        error->Send(ctx, {});
      }
    };
    Queue::gl().Push(std::move(task));

    srcs_.clear();
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;

  GLenum t_ = 0;

  std::vector<std::shared_ptr<const Value::String>> srcs_;
};


class DrawArrays final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<DrawArrays>;

  static inline TypeInfo kType = TypeInfo::New<Owner>(
      "GL/DrawArrays", "A node that call glDrawArrays",
      {typeid(iface::Node)});

  static std::string title() noexcept { return "glDrawArrays"; }

  static inline const std::vector<SockMeta> kInSocks = {
    { .name = "clear", .type = SockMeta::kPulse, .trigger = true, },

    { .name = "prog", .type = SockMeta::kData, },
    { .name = "fb",   .type = SockMeta::kData, },
    { .name = "vao",  .type = SockMeta::kData, },

    { .name = "uniforms", .type = SockMeta::kNamedValue, },

    { .name = "viewport", .type = SockMeta::kVec4, },
    { .name = "mode",     .type = SockMeta::kStringOption,
      .stringOptions = gl::GetEnumNames(gl::kDrawModes),
    },

    { .name = "first", .type = SockMeta::kInteger, .def = Value::Integer{0}, },
    { .name = "count", .type = SockMeta::kInteger, },

    { .name = "exec", .type = SockMeta::kPulse, .trigger = true, },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { .name = "done", .type = SockMeta::kPulse, },
  };

  DrawArrays() = delete;
  DrawArrays(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:
      Clear();
      return;
    case 1:
      prog_ = v.dataPtr<gl::Program>();
      return;
    case 2:
      fb_ = v.dataPtr<gl::Framebuffer>();
      return;
    case 3:
      vao_ = v.dataPtr<gl::VertexArray>();
      return;
    case 4:
      Uniform(std::move(v));
      return;
    case 5:
      viewport_ = v.tuple().float4();
      return;
    case 6:
      mode_ = gl::ParseDrawMode(v.string()).gl;
      return;
    case 7:
      first_ = v.integer<GLint>(0);
      return;
    case 8:
      count_ = v.integer<GLsizei>(0);
      return;
    case 9:
      Exec();
      return;
    }
    assert(false);
  }
  void Clear() noexcept {
    prog_ = nullptr;
    fb_   = nullptr;
    vao_  = nullptr;

    uniforms_.clear();
    viewport_ = {0, 0, 0, 0};
    mode_ = 0;
    first_ = 0;
    count_ = 0;
  }
  void Exec() {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    if (!prog_) {
      throw Exception("prog is not specified");
    }
    if (!fb_) {
      throw Exception("framebuffer is not specified");
    }
    if (!vao_) {
      throw Exception("vao is not specified");
    }
    if (mode_ == 0) {
      throw Exception("mode is not specified");
    }
    // TODO validate vertex count

    auto& done = owner_->sharedOut(0);
    if (count_ == 0) {
      done->Send(ctx, {});
      return;
    }

    auto task = [owner = owner_, ctx, done,
                 prog     = prog_,
                 fb       = fb_,
                 vao      = vao_,
                 uni      = uniforms_,
                 viewport = viewport_,
                 mode     = mode_,
                 first    = first_,
                 count    = count_]() {
      glUseProgram(prog->id());
      glBindFramebuffer(GL_FRAMEBUFFER, fb->id());
      glBindVertexArray(vao->id());

      for (auto& u : uni) {
        try {
          GL_SetUniform(prog->id(), u.first, u.second);
        } catch (Exception& e) {
          ctx->Notify(owner, e.msg());
        }
      }

      glViewport(static_cast<GLint>(viewport[0]),
                 static_cast<GLint>(viewport[1]),
                 static_cast<GLsizei>(viewport[2]),
                 static_cast<GLsizei>(viewport[3]));
      glDrawArrays(mode, static_cast<GLint>(first), static_cast<GLsizei>(count));
      done->Send(ctx, {});

      glBindVertexArray(0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glUseProgram(0);

      assert(glGetError() == GL_NO_ERROR);
    };
    Queue::gl().Push(std::move(task));
  }
  void Uniform(Value&& v) {
    const auto& tup = v.tuple();

    const auto& key = tup[0];
    const auto& val = tup[1];

    IndexOrName idx_or_name;
    if (key.isInteger()) {
      const auto idx = key.integer();
      if (idx < 0) throw Exception("invalid uniform index");
      idx_or_name = static_cast<GLint>(idx);
    } else if (key.isString()) {
      idx_or_name = key.string();
    } else {
      throw Exception("integer or string is allowed for uniform key");
    }

    const bool valid = val.isInteger() || val.isScalar();
    if (!valid) {
      throw Exception("integer or scalar is allowed for uniform value");
    }
    uniforms_[idx_or_name] = val;
  }

 private:
  using IndexOrName = std::variant<GLint, std::string>;

  Owner* owner_;

  std::weak_ptr<Context> ctx_;

  std::shared_ptr<gl::Program> prog_;
  std::shared_ptr<gl::Framebuffer> fb_;
  std::shared_ptr<gl::VertexArray> vao_;

  std::unordered_map<IndexOrName, Value> uniforms_;

  linalg::float4 viewport_ = {0, 0, 0, 0};
  GLenum  mode_ = 0;
  GLint   first_ = 0;
  GLsizei count_ = 0;


  static void GL_SetUniform(GLuint prog, const IndexOrName& key, const Value& val) {
    GLint idx;
    if (std::holds_alternative<GLint>(key)) {
      idx = std::get<GLint>(key);
    } else {
      const auto& name = std::get<std::string>(key);
      idx = glGetUniformLocation(prog, name.c_str());
      if (idx == -1) {
        throw Exception("unknown uniform name: "+name);
      }
    }
    if (val.isInteger()) {
      glUniform1i(idx, val.integer<GLint>());
    } else if (val.isScalar()) {
      glUniform1f(idx, static_cast<float>(val.scalar()));
    } else {
      assert(false);
    }
  }
};


class Preview final : public File, public iface::DirItem, public iface::Node {
 public:
  static inline TypeInfo kType = TypeInfo::New<Preview>(
      "GL/Preview", "provides OpenGL texture preview window",
      {typeid(iface::DirItem)});

  static inline const SockMeta kInTex = {
    .name = "tex", .type = SockMeta::kData, .trigger = true,
  };

  Preview(Env* env, bool shown = false) noexcept :
      File(&kType, env), DirItem(DirItem::kNone), Node(Node::kNone),
      shown_(shown) {
    auto task_tex = [this](auto& ctx, auto&& v) {
      try {
        tex_ = v.template dataPtr<gl::Texture>();
      } catch (Exception& e) {
        ctx->Notify(this, "error while handling 'tex': "+e.msg());
      }
    };
    in_.emplace_back(new NodeLambdaInSock(this, &kInTex, std::move(task_tex)));
  }

  Preview(Env* env, const msgpack::object& obj)
  try : Preview(env, obj.as<bool>()) {
  } catch (msgpack::type_error&) {
    throw DeserializeException("broken GL/Preview");
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(shown_);
  }
  std::unique_ptr<File> Clone(Env* env) const noexcept override {
    return std::make_unique<Preview>(env, shown_);
  }

  void Update(Event& ev) noexcept override {
    if (gui::BeginWindow(this, "OpenGL Preview", ev, &shown_)) {
      if (!tex_) {
        ImGui::TextUnformatted("texture is not specified");
      } else if (tex_->id() == 0) {
        ImGui::TextUnformatted("texture is not ready");
      } else {
        const auto id = (ImTextureID) static_cast<uintptr_t>(tex_->id());
        ImGui::Image(id, ImGui::GetContentRegionAvail());
      }
    }
    gui::EndWindow();
  }

  void* iface(const std::type_index& t) noexcept override {
    return PtrSelector<iface::DirItem, iface::Node>(t).Select(this);
  }

 private:
  // permanentized
  bool shown_;

  // volatile
  std::shared_ptr<gl::Texture> tex_;
};


}  // namespace kingtaker

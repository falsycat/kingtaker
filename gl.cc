#include <cassert>
#include <cinttypes>

#include <imgui.h>
#include <imgui_internal.h>
#include <ImNodes.h>

#include "kingtaker.hh"

#include "iface/dir.hh"
#include "iface/node.hh"

#include "util/gl.hh"
#include "util/gui.hh"
#include "util/node.hh"
#include "util/notify.hh"
#include "util/ptr_selector.hh"
#include "util/value.hh"

namespace kingtaker {

static void GetResolution(const Value& v, int32_t& w, int32_t& h) {
  constexpr int32_t kMaxReso = 4096;

  const auto size = v.vec2();
  w = static_cast<int32_t>(size.x);
  h = static_cast<int32_t>(size.y);
  if (w <= 0 || h <= 0 || w > kMaxReso || h > kMaxReso) {
    throw Exception("out of range");
  }
}


class TextureFactory final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<TextureFactory>;

  static inline TypeInfo type_ = TypeInfo::New<Owner>(
      "GL/TextureFactory", "A node that creates renderbuffer object",
      {typeid(iface::Node)});

  static constexpr const char* kTitle = "GL Tex Factory";

  static inline const std::vector<SockMeta> kInSocks = {
    { "CLK", "", kPulseButton },
    { "CLR", "", kPulseButton },

    { "reso",    "" },
    { "format",  "" },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "out",  "" },
    { "errr", "" },
  };

  TextureFactory() = delete;
  TextureFactory(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:  Exec();  return;
    case 1:  Clear(); return;
    default: Set(idx, std::move(v));
    }
  }
  void Exec() {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    auto out  = owner_->out()[0];
    auto errr = owner_->out()[1];

    try {
      // get resolution
      int32_t w, h;
      try {
        GetResolution(in(2), w, h);
      } catch (Exception& e) {
        throw Exception("invalid reso: "+e.msg());
      }

      // get format
      GLenum fmt;
      try {
        const auto idx = gl::ParseFormat<Exception>(in(3).string());
        fmt = gl::kFormats[idx].gl;
      } catch (Exception& e) {
        throw Exception("invalid format: "+e.msg());
      }

      // create tex
      auto tex = gl::Texture::Create(GL_TEXTURE_2D);
      // TODO set metadata
      auto task = [fmt = static_cast<GLint>(fmt), w, h, tex, ctx, out]() {
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
                     0, fmt, w, h, 0, exfmt, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);

        out->Send(ctx, std::dynamic_pointer_cast<Value::Data>(tex));
      };
      Queue::gl().Push(std::move(task));

    } catch (Exception& e) {
      notify::Error(owner_->path(), owner_, e.msg());
      errr->Send(ctx, {});
      return;
    }
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;
};


class RenderbufferFactory final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<RenderbufferFactory>;

  static inline TypeInfo type_ = TypeInfo::New<Owner>(
      "GL/RenderbufferFactory", "A node that creates renderbuffer object",
      {typeid(iface::Node)});

  static constexpr const char* kTitle = "GL RB Factory";

  static inline const std::vector<SockMeta> kInSocks = {
    { "CLK", "", kPulseButton },
    { "CLR", "", kPulseButton },

    { "reso",    "" },
    { "format",  "" },
    { "samples", "" },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "out",  "" },
    { "errr", "" },
  };

  RenderbufferFactory() = delete;
  RenderbufferFactory(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:  Exec();  return;
    case 1:  Clear(); return;
    default: Set(idx, std::move(v));
    }
  }
  void Exec() {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    auto out  = owner_->out()[0];
    auto errr = owner_->out()[1];

    try {
      // get resolution
      int32_t w, h;
      try {
        GetResolution(in(2), w, h);
      } catch (Exception& e) {
        throw Exception("invalid reso: "+e.msg());
      }

      // get format
      GLenum fmt;
      try {
        const auto idx = gl::ParseFormat<Exception>(in(3).string());
        fmt = gl::kFormats[idx].gl;
      } catch (Exception& e) {
        throw Exception("invalid format: "+e.msg());
      }

      // get samples
      GLsizei samples;
      try {
        samples = static_cast<GLsizei>(in(4).integer());
        if (samples < 0 || samples >= 32) {
          throw Exception("out of range");
        }
      } catch (Exception& e) {
        throw Exception("invalid samples: "+e.msg());
      }

      // create rb
      auto rb = gl::Renderbuffer::Create(GL_RENDERBUFFER);
      // TODO set metadata
      auto task = [samples, fmt, w, h, rb, ctx, out]() {
        glBindRenderbuffer(GL_RENDERBUFFER, rb->id());
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, fmt, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        out->Send(ctx, std::dynamic_pointer_cast<Value::Data>(rb));
      };
      Queue::gl().Push(std::move(task));

    } catch (Exception& e) {
      notify::Error(owner_->path(), owner_, e.msg());
      errr->Send(ctx, {});
      return;
    }
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;
};


class FramebufferFactory final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<FramebufferFactory>;

  static inline TypeInfo type_ = TypeInfo::New<Owner>(
      "GL/FramebufferFactory", "A node that creates framebuffer object",
      {typeid(iface::Node)});

  static constexpr const char* kTitle = "FB Factory";

  static inline const std::vector<SockMeta> kInSocks = {
    { "CLK", "", kPulseButton },
    { "CLR", "", kPulseButton },

    { "reso",    "" },
    { "color0",  "" },
    { "color1",  "" },
    { "color2",  "" },
    { "color3",  "" },
    { "color4",  "" },
    { "color5",  "" },
    { "color6",  "" },
    { "color7",  "" },
    { "depth",   "" },
    { "stencil", "" },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "out",  "" },
    { "errr", "" },
  };

  FramebufferFactory() = delete;
  FramebufferFactory(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0:  Exec();  return;
    case 1:  Clear(); return;
    default: Set(idx, std::move(v));
    }
  }
  void Exec() {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    auto out  = owner_->out()[0];
    auto errr = owner_->out()[1];

    // get resolution
    int32_t w, h;
    try {
      GetResolution(in(2), w, h);
    } catch (Exception& e) {
      notify::Error(owner_->path(), owner_, "invalid reso: "+e.msg());
      return;
    }

    // create fb and attach inputs
    auto fb = gl::Framebuffer::Create(GL_FRAMEBUFFER);
    for (const auto& at : gl::kAttachments) {
      try {
        const auto& value = in(3+at.idx);
        if (value.isPulse()) continue;

        const auto& data = value.dataPtr();

        // validate tex or rb
        auto tex = std::dynamic_pointer_cast<gl::Texture>(data);
        auto rb  = std::dynamic_pointer_cast<gl::Renderbuffer>(data);
        if (!tex && !rb) {
          throw Exception(at.name+" is not pulse, gl::Texture, or gl::Renderbuffer, "
                         "treated as unspecified");
        }
        if (tex && tex->gl() != GL_TEXTURE_2D) {
          throw Exception(at.name+" is gl::Texture, but not GL_TEXTURE_2D, "
                         "treated as unspecified");
        }

        // TODO tex or rb size validation
        // TODO tex or rb type validation

        // attach to fb
        auto task = [&at, fb, tex, rb]() {
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

      } catch (Exception& e) {
        notify::Warn(owner_->path(), owner_,
                     "error while handling attachment "+at.name+": "+e.msg());
        continue;
      }
    }

    // check status and emit result
    auto task = [owner = owner_, path = owner_->path(), fb, ctx, out, errr]() {
      glBindFramebuffer(GL_FRAMEBUFFER, fb->id());
      const auto stat = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (stat == GL_FRAMEBUFFER_COMPLETE) {
        out->Send(ctx, std::dynamic_pointer_cast<Value::Data>(fb));
      } else {
        notify::Error(path, owner, "broken framebuffer ("+std::to_string(stat)+")");
        errr->Send(ctx, {});
      }
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    };
    Queue::gl().Push(std::move(task));
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;
};


class VertexArrayFactory final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<VertexArrayFactory>;

  static inline TypeInfo type_ = TypeInfo::New<Owner>(
      "GL/VertexArrayFactory", "A node that creates vertex array object",
      {typeid(iface::Node)});

  static constexpr const char* kTitle = "GL VAO Factory";

  static inline const std::vector<SockMeta> kInSocks = {
    { "CLK", "", kPulseButton },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "out", "" },
  };

  VertexArrayFactory() = delete;
  VertexArrayFactory(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t, Value&&) {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    auto out = owner_->out()[0];

    // create vao
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


class ProgramFactory final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<ProgramFactory>;

  static inline TypeInfo type_ = TypeInfo::New<Owner>(
      "GL/ProgramFactory", "A node that links program object",
      {typeid(iface::Node)});

  static constexpr const char* kTitle = "GL Program Factory";

  static inline const std::vector<SockMeta> kInSocks = {
    { "LINK",    "", kPulseButton },
    { "CLR",     "", kPulseButton },
    { "shaders", "" },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "out",  "" },
    { "errr", "" },
  };

  ProgramFactory() = delete;
  ProgramFactory(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0: Link();                     return;
    case 1: Clear();                    return;
    case 2: AttachShader(std::move(v)); return;
    default: assert(false);
    }
  }
  void Clear() noexcept {
    dirty_ = false;
    prog_  = nullptr;
  }
  void AttachShader(Value&& v) noexcept {
    if (!prog_) {
      if (dirty_) {
        notify::Error(owner_->path(), owner_, "clear before attaching");
        return;
      }
      dirty_ = true;
      prog_  = gl::Program::Create(0);
    }

    try {
      const auto shader = v.dataPtr<gl::Shader>();

      auto task = [prog = prog_, shader]() {
        glAttachShader(prog->id(), shader->id());
        assert(glGetError() == GL_NO_ERROR);
      };
      Queue::gl().Push(std::move(task));

    } catch (Exception& e) {
      notify::Warn(owner_->path(), owner_,
                   "skipped attaching shader because of error: "+e.msg());
      return;
    }
  }
  void Link() noexcept {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    auto out  = owner_->out()[0];
    auto errr = owner_->out()[1];

    if (!prog_) {
      notify::Error(owner_->path(), owner_, "no shaders are attached");
      errr->Send(ctx, {});
      return;
    }

    auto task = [owner = owner_, path = owner_->path(),
                 prog = prog_, ctx, out, errr]() {
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

        notify::Error(path, owner, buf);
        errr->Send(ctx, {});
      }
    };
    Queue::gl().Push(std::move(task));

    prog_ = nullptr;
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;

  bool dirty_ = true;

  std::shared_ptr<gl::Program> prog_;
};


class ShaderFactory final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<ShaderFactory>;

  static inline TypeInfo type_ = TypeInfo::New<Owner>(
      "GL/ShaderFactory", "A node that compiles shader",
      {typeid(iface::Node)});

  static constexpr const char* kTitle = "GL Shader Factory";

  static inline const std::vector<SockMeta> kInSocks = {
    { "CLK",  "", kPulseButton },
    { "CLR",  "", kPulseButton },
    { "type", "" },
    { "src",  "" },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "out",  "" },
    { "errr", "" },
  };

  ShaderFactory() = delete;
  ShaderFactory(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0: Exec();  return;
    case 1: Clear(); return;
    default: Set(idx, std::move(v));
    }
  }
  void Exec() noexcept {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    auto out  = owner_->out()[0];
    auto errr = owner_->out()[1];

    try {
      const auto type = in(2).string();
      const auto src  = in(3).stringPtr();

      GLenum t = 0;
      if (type == "vertex")   t = GL_VERTEX_SHADER;
      if (type == "geometry") t = GL_GEOMETRY_SHADER;
      if (type == "fragment") t = GL_FRAGMENT_SHADER;
      if (t == 0) throw Exception("unknown shader type: "+type);

      auto shader = gl::Shader::Create(t);
      auto task = [owner = owner_, path = owner_->path(),
                   shader, src, ctx, out, errr]() {
        const auto id = shader->id();

        const char* ptr = src->c_str();
        glShaderSource(id, 1, &ptr, nullptr);
        glCompileShader(id);

        GLint compiled;
        glGetShaderiv(id, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_TRUE) {
          out->Send(ctx, std::dynamic_pointer_cast<Value::Data>(shader));
        } else {
          GLsizei len = 0;
          char buf[1024];
          glGetShaderInfoLog(id, sizeof(buf), &len, buf);

          notify::Error(path, owner, buf);
          errr->Send(ctx, {});
        }
      };
      Queue::gl().Push(std::move(task));

    } catch (Exception& e) {
      notify::Warn(owner_->path(), owner_, e.msg());
      errr->Send(ctx, {});
      return;
    }
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;
};


class DrawArrays final : public LambdaNodeDriver {
 public:
  using Owner = LambdaNode<DrawArrays>;

  static inline TypeInfo type_ = TypeInfo::New<Owner>(
      "GL/DrawArrays", "A node that call glDrawArrays",
      {typeid(iface::Node)});

  static constexpr const char* kTitle = "glDrawArrays";

  static inline const std::vector<SockMeta> kInSocks = {
    { "CLK", "", kPulseButton },
    { "CLR", "", kPulseButton },
    { "prog",     "" },
    { "fb",       "" },
    { "vao",      "" },
    { "viewport", "" },
    { "mode",     "" },
    { "first",    "" },
    { "count",    "" },
  };
  static inline const std::vector<SockMeta> kOutSocks = {
    { "done", "" },
    { "errr", "" },
  };

  DrawArrays() = delete;
  DrawArrays(Owner* o, const std::weak_ptr<Context>& ctx) noexcept :
      owner_(o), ctx_(ctx) {
  }

  void Handle(size_t idx, Value&& v) {
    switch (idx) {
    case 0: Exec();  return;
    case 1: Clear(); return;
    default: Set(idx, std::move(v));
    }
  }
  void Exec() noexcept {
    auto ctx = ctx_.lock();
    if (!ctx) return;

    auto done = owner_->out()[0];
    auto errr = owner_->out()[1];

    try {
      // get objects
      const auto prog = in(2).dataPtr<gl::Program>();
      const auto fb   = in(3).dataPtr<gl::Framebuffer>();
      const auto vao  = in(4).dataPtr<gl::VertexArray>();

      // get viewport
      const auto& viewport = in(5).vec4();

      // get mode
      const auto& mode  = in(6).string();
      GLenum m = 0;
      if (mode == "triangles") m = GL_TRIANGLES;
      // TODO
      if (m == 0) throw Exception("unknown draw mode: "+mode);

      // get vertices
      const auto first = in(7).integer();
      const auto count = in(8).integer();
      try {
        if (first < 0 || count < 0) {
          throw Exception("out of range");
        }
        // TODO validate vertex count
      } catch(Exception& e) {
        throw Exception("invalid first/count param: "+e.msg());
      }

      auto task = [prog, fb, vao, viewport, m, first, count, ctx, done]() {
        glUseProgram(prog->id());
        glBindFramebuffer(GL_FRAMEBUFFER, fb->id());
        glBindVertexArray(vao->id());
        glViewport(static_cast<GLint>(viewport[0]),
                   static_cast<GLint>(viewport[1]),
                   static_cast<GLsizei>(viewport[2]),
                   static_cast<GLsizei>(viewport[3]));
        glDrawArrays(m, static_cast<GLint>(first), static_cast<GLsizei>(count));
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glUseProgram(0);

        assert(glGetError() == GL_NO_ERROR);
        done->Send(ctx, {});
      };
      Queue::gl().Push(std::move(task));

    } catch (Exception& e) {
      notify::Error(owner_->path(), owner_, e.msg());
      errr->Send(ctx, {});
      return;
    }
  }

 private:
  Owner* owner_;

  std::weak_ptr<Context> ctx_;
};


class Preview final : public File, public iface::DirItem {
 public:
  static inline TypeInfo type_ = TypeInfo::New<Preview>(
      "GL/Preview", "provides OpenGL texture preview window",
      {typeid(iface::DirItem)});

  Preview(const std::shared_ptr<Env>& env, bool shown = false) noexcept :
      File(&type_, env), DirItem(DirItem::kNone), shown_(shown) {
  }

  static std::unique_ptr<File> Deserialize(
      const msgpack::object& obj, const std::shared_ptr<Env>& env) {
    try {
      return std::make_unique<Preview>(env, obj.as<bool>());
    } catch (msgpack::type_error&) {
      throw DeserializeException("broken GL/Preview");
    }
  }
  void Serialize(Packer& pk) const noexcept override {
    pk.pack(shown_);
  }
  std::unique_ptr<File> Clone(const std::shared_ptr<Env>& env) const noexcept override {
    return std::make_unique<Preview>(env, shown_);
  }

  void Update(RefStack& ref, Event& ev) noexcept override {
    if (gui::BeginWindow(this, "OpenGL Preview", ref, ev, &shown_)) {
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
    return PtrSelector<iface::DirItem>(t).Select(this);
  }

 private:
  bool shown_;

  std::shared_ptr<gl::Texture> tex_;


  class Show final : public LambdaNodeDriver {
   public:
    using Owner = LambdaNode<Show>;

    static inline TypeInfo type_ = TypeInfo::New<Owner>(
        "GL/Preview/Show", "Shows received texture in a preview",
        {typeid(iface::Node)});

    static constexpr const char* kTitle = "GL/Preview/Show";

    static inline const std::vector<SockMeta> kInSocks = {
      { "CLK", "", kPulseButton },
      { "CLR", "", kPulseButton },

      { "path", "" },
      { "tex",  "" },
    };
    static inline const std::vector<SockMeta> kOutSocks = {};

    Show() = delete;
    Show(Owner* o, const std::weak_ptr<Context>&) noexcept : owner_(o) {
    }

    void Handle(size_t idx, Value&& v) {
      switch (idx) {
      case 0:  Exec();  return;
      case 1:  Clear(); return;
      default: Set(idx, std::move(v));
      }
    }
    void Exec() {
      try {
        auto path = File::ParsePath(in(2).string());

        auto target_file = &*RefStack().Resolve(owner_->path()).Resolve(path);
        auto target      = dynamic_cast<Preview*>(target_file);
        if (!target) {
          throw Exception("target is not a preview");
        }

        auto tex = in(3).dataPtr<gl::Texture>();
        if (!tex || tex->gl() != GL_TEXTURE_2D) {
          throw Exception("tex is not a GL_TEXTURE_2D");
        }
        target->tex_ = tex;

      } catch (Exception& e) {
        notify::Error(owner_->path(), owner_, e.msg());
      }
    }

   private:
    Owner* owner_;
  };
};



}  // namespace kingtaker

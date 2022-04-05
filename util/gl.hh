#pragma once

#include <cassert>
#include <memory>
#include <mutex>
#include <vector>

#include <GL/glew.h>

#include "kingtaker.hh"

#include "util/value.hh"


namespace kingtaker::gl {

class Exception : public HeavyException {
 public:
  Exception(std::string_view msg, Loc loc = Loc::current()) noexcept :
      HeavyException(msg, loc) {
  }
};


struct Enum final {
  size_t      idx;
  GLenum      gl;
  std::string name;
};
template <typename E = DeserializeException>
const Enum& ParseEnum(const char* name, std::span<const Enum> list, std::string_view v) {
  for (const auto& e : list) {
    if (e.name == v) return e;
  }
  throw E("unknown OpenGL "s+name+": "+std::string(v));
}

static inline const std::vector<Enum> kAttachments = {
  { 0, GL_COLOR_ATTACHMENT0,  "color0",  },
  { 1, GL_COLOR_ATTACHMENT1,  "color1",  },
  { 2, GL_COLOR_ATTACHMENT2,  "color2",  },
  { 3, GL_COLOR_ATTACHMENT3,  "color3",  },
  { 4, GL_COLOR_ATTACHMENT4,  "color4",  },
  { 5, GL_COLOR_ATTACHMENT5,  "color5",  },
  { 6, GL_COLOR_ATTACHMENT6,  "color6",  },
  { 7, GL_COLOR_ATTACHMENT7,  "color7",  },
  { 8, GL_DEPTH_ATTACHMENT,   "depth",   },
  { 9, GL_STENCIL_ATTACHMENT, "stencil", },
};
template <typename E = DeserializeException>
const Enum& ParseAttachment(std::string_view name) {
  return ParseEnum<E>("attachment", kAttachments, name);
}

static inline const std::vector<Enum> kFormats = {
  { 0, GL_RGBA8,              "RGBA8"    },
  { 1, GL_RGB8,               "RGB8"     },
  { 2, GL_RG8,                "RG8"      },
  { 3, GL_R8,                 "R8"       },
  { 4, GL_DEPTH_COMPONENT32F, "depth32f" },
  { 5, GL_DEPTH_COMPONENT24,  "depth24"  },
  { 6, GL_DEPTH_COMPONENT16,  "depth16"  },
};
template <typename E = DeserializeException>
const Enum& ParseFormat(std::string_view name) {
  return ParseEnum<E>("format", kFormats, name);
}

static inline const std::vector<Enum> kShaderTypes = {
  { 0, GL_VERTEX_SHADER,   "vertex"   },
  { 1, GL_GEOMETRY_SHADER, "geometry" },
  { 2, GL_FRAGMENT_SHADER, "fragment" },
};
template <typename E = DeserializeException>
const Enum& ParseShaderType(std::string_view name) {
  return ParseEnum<E>("shader type", kShaderTypes, name);
}

static inline const std::vector<Enum> kDrawModes = {
  { 0, GL_TRIANGLES, "triangles" },
  // TODO
};
template <typename E = DeserializeException>
const Enum& ParseDrawMode(std::string_view name) {
  return ParseEnum<E>("draw mode", kDrawModes, name);
}


class Obj : public Value::Data {
 public:
  Obj() = delete;
  Obj(const char* name, GLenum gl) noexcept : Data(name), gl_(gl) {
  }
  Obj(const Obj&) = delete;
  Obj(Obj&&) = delete;
  Obj& operator=(const Obj&) = delete;
  Obj& operator=(Obj&&) = delete;

  GLenum gl() const noexcept { return gl_; }
  GLuint id() const noexcept { return id_; }

  void id(GLuint i) noexcept { assert(id_ == 0); id_ = i; }

 private:
  GLenum gl_ = 0;
  GLuint id_ = 0;
};

template <typename T>
class ObjImpl final : public Obj, public T {
 public:
  friend inline void HandleAll();

  // max size of array passed to T::Generate
  static constexpr size_t kMaxGenerateSize = 256;

  static std::shared_ptr<ObjImpl> Create(GLenum t) noexcept {
    std::unique_lock<std::mutex> k(mtx_);
    gen_.emplace_back(new ObjImpl(t));
    return gen_.back();
  }

  ~ObjImpl() noexcept {
    std::unique_lock<std::mutex> k(mtx_);
    if (id()) del_.push_back(id());
  }

 private:
  static inline std::mutex                            mtx_;
  static inline std::vector<std::shared_ptr<ObjImpl>> gen_;
  static inline std::vector<GLuint>                   del_;

  static void Handle() {
    std::unique_lock<std::mutex> k(mtx_);

    // create new objects
    for (size_t i = 0; i < gen_.size(); i+=kMaxGenerateSize) {
      T::Generate({ &gen_[i], gen_.size()-i });
    }
    for (auto& obj : gen_) {
      if (obj->id() == 0) throw Exception(obj->type()+" allocation failure"s);
    }
    gen_.clear();

    // delete unused objects
    if (del_.size()) {
      T::Delete(del_);
      del_.clear();
    }

    assert(glGetError() == GL_NO_ERROR);
  }

  ObjImpl(GLenum t) noexcept : Obj(T::kName, t) {
  }
};


class Buffer_ {
 protected:
  static constexpr const char* kName = "kingtaker::gl::Buffer";

  static void Generate(
      std::span<std::shared_ptr<ObjImpl<Buffer_>>> objs) noexcept {
    GLuint temp[ObjImpl<Buffer_>::kMaxGenerateSize];
    glGenBuffers(static_cast<GLsizei>(objs.size()), temp);
    for (size_t i = 0; i < objs.size(); ++i) {
      objs[i]->id(temp[i]);
    }
  }
  static void Delete(std::span<GLuint> ids) noexcept {
    glDeleteBuffers(static_cast<GLsizei>(ids.size()), ids.data());
  }
};
using Buffer = ObjImpl<Buffer_>;


class Texture_ {
 protected:
  static constexpr const char* kName = "kingtaker::gl::Texture";

  static void Generate(
      std::span<std::shared_ptr<ObjImpl<Texture_>>> objs) noexcept {
    GLuint temp[ObjImpl<Texture_>::kMaxGenerateSize];
    glGenTextures(static_cast<GLsizei>(objs.size()), temp);
    for (size_t i = 0; i < objs.size(); ++i) {
      objs[i]->id(temp[i]);
    }
  }
  static void Delete(std::span<GLuint> ids) noexcept {
    glDeleteTextures(static_cast<GLsizei>(ids.size()), ids.data());
  }
};
using Texture = ObjImpl<Texture_>;


class Framebuffer_ {
 protected:
  static constexpr const char* kName = "kingtaker::gl::Framebuffer";

  static void Generate(
      std::span<std::shared_ptr<ObjImpl<Framebuffer_>>> objs) noexcept {
    GLuint temp[ObjImpl<Framebuffer_>::kMaxGenerateSize];
    glGenFramebuffers(static_cast<GLsizei>(objs.size()), temp);
    for (size_t i = 0; i < objs.size(); ++i) {
      objs[i]->id(temp[i]);
    }
  }
  static void Delete(std::span<GLuint> ids) noexcept {
    glDeleteFramebuffers(static_cast<GLsizei>(ids.size()), ids.data());
  }
};
using Framebuffer = ObjImpl<Framebuffer_>;


class Renderbuffer_ {
 protected:
  static constexpr const char* kName = "kingtaker::gl::Renderbuffer";

  static void Generate(
      std::span<std::shared_ptr<ObjImpl<Renderbuffer_>>> objs) noexcept {
    GLuint temp[ObjImpl<Renderbuffer_>::kMaxGenerateSize];
    glGenRenderbuffers(static_cast<GLsizei>(objs.size()), temp);
    for (size_t i = 0; i < objs.size(); ++i) {
      objs[i]->id(temp[i]);
    }
  }
  static void Delete(std::span<GLuint> ids) noexcept {
    glDeleteRenderbuffers(static_cast<GLsizei>(ids.size()), ids.data());
  }
};
using Renderbuffer = ObjImpl<Renderbuffer_>;


class VertexArray_ {
 protected:
  static constexpr const char* kName = "kingtaker::gl::VertexArray";

  static void Generate(
      std::span<std::shared_ptr<ObjImpl<VertexArray_>>> objs) noexcept {
    GLuint temp[ObjImpl<VertexArray_>::kMaxGenerateSize];
    glGenVertexArrays(static_cast<GLsizei>(objs.size()), temp);
    for (size_t i = 0; i < objs.size(); ++i) {
      objs[i]->id(temp[i]);
    }
  }
  static void Delete(std::span<GLuint> ids) noexcept {
    glDeleteVertexArrays(static_cast<GLsizei>(ids.size()), ids.data());
  }
};
using VertexArray = ObjImpl<VertexArray_>;


class Sampler_ {
 protected:
  static constexpr const char* kName = "kingtaker::gl::Sampler";

  static void Generate(
      std::span<std::shared_ptr<ObjImpl<Sampler_>>> objs) noexcept {
    GLuint temp[ObjImpl<Sampler_>::kMaxGenerateSize];
    glGenSamplers(static_cast<GLsizei>(objs.size()), temp);
    for (size_t i = 0; i < objs.size(); ++i) {
      objs[i]->id(temp[i]);
    }
  }
  static void Delete(std::span<GLuint> ids) noexcept {
    glDeleteSamplers(static_cast<GLsizei>(ids.size()), ids.data());
  }
};
using Sampler = ObjImpl<Sampler_>;


class Program_ {
 protected:
  static constexpr const char* kName = "kingtaker::gl::Program";

  static void Generate(
      std::span<std::shared_ptr<ObjImpl<Program_>>> objs) noexcept {
    for (auto& obj : objs) {
      obj->id(glCreateProgram());
    }
  }
  static void Delete(std::span<GLuint> ids) noexcept {
    for (auto id : ids) glDeleteProgram(id);
  }
};
using Program = ObjImpl<Program_>;


class Shader_ {
 protected:
  static constexpr const char* kName = "kingtaker::gl::Shader";

  static void Generate(
      std::span<std::shared_ptr<ObjImpl<Shader_>>> objs) noexcept {
    for (auto& obj : objs) {
      obj->id(glCreateShader(obj->gl()));
    }
  }
  static void Delete(std::span<GLuint> ids) noexcept {
    for (auto id : ids) glDeleteShader(id);
  }
};
using Shader = ObjImpl<Shader_>;


inline void HandleAll() {
  Buffer::Handle();
  Texture::Handle();
  Framebuffer::Handle();
  Renderbuffer::Handle();
  VertexArray::Handle();
  Sampler::Handle();
  Program::Handle();
  Shader::Handle();
}

}  // namespace kingtaker::gl

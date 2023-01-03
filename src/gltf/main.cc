#include <zukou.h>

#include <linux/input.h>
#include <sys/mman.h>

#include <cstring>
#include <deque>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/vec3.hpp>
#include <iostream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gltf.color.fragment.h"
#include "gltf.texture.fragment.h"
#include "gltf.vertex.h"
#include "jpeg-texture.h"
#include "tiny_gltf.h"

class Viewer : public zukou::IBoundedDelegate, public zukou::ISystemDelegate
{
 public:
  Viewer(tinygltf::Model *model, std::filesystem::path parent_dir)
      : system_(this),
        bounded_(&system_, this),
        pool_(&system_),
        vertex_shader_(&system_),
        texture_fragment_shader_(&system_),
        color_fragment_shader_(&system_),
        sampler_(&system_),
        model_(model),
        parent_dir_(parent_dir)
  {}

  bool Init(glm::vec3 half_size)
  {
    if (!system_.Init()) return false;
    if (!bounded_.Init(half_size)) return false;
    return true;
  }

  void Configure(glm::vec3 half_size, uint32_t serial) override
  {
    if (!this->Setup()) {
      std::cerr << "Failed to setup buffer" << std::endl;
      return;
    }

    bounded_.AckConfigure(serial);

    matrix_stack_.push_back(glm::mat4(1));

    this->RenderScene();

    this->SetupRegion(half_size);
  }

  void RayEnter(uint32_t /*serial*/, zukou::VirtualObject * /*virtual_object*/,
      glm::vec3 /*origin*/, glm::vec3 /*direction*/) override
  {
    bounded_.Commit();
  };

  void RayLeave(
      uint32_t /*serial*/, zukou::VirtualObject * /*virtual_object*/) override
  {
    bounded_.Commit();
  };

  void RayButton(uint32_t serial, uint32_t /*time*/, uint32_t button,
      bool pressed) override
  {
    if (button == BTN_LEFT && pressed) {
      bounded_.Move(serial);
    }
  };

  bool Run() { return system_.Run(); }

 private:
  zukou::System system_;
  zukou::Bounded bounded_;

  zukou::ShmPool pool_;

  std::unordered_map<std::string, zukou::RenderingUnit *> rendering_unit_map_;
  std::unordered_map<std::string, zukou::GlBaseTechnique *> base_technique_map_;

  std::unordered_map<int, zukou::GlBuffer *> gl_vertex_buffer_map_;

  zukou::GlShader vertex_shader_;
  zukou::GlShader texture_fragment_shader_;
  zukou::GlShader color_fragment_shader_;
  std::unordered_map<std::string, zukou::GlProgram *> program_map_;

  zukou::GlSampler sampler_;

  tinygltf::Model *model_;

  std::filesystem::path parent_dir_;

  std::unordered_map<int, JpegTexture *> texture_map_;

  std::deque<glm::mat4> matrix_stack_;

 private:
  glm::mat4 CalculateLocalModel()
  {
    glm::mat4 value(1);
    for (auto matrix : matrix_stack_) {
      value *= matrix;
    }
    return value;
  }

  bool Setup()
  {
    if (model_->buffers.size() == 0) {
      std::cerr << "buffer size is zero." << std::endl;
      return false;
    }
    if (model_->buffers.size() > 1) {
      std::cerr << "TODO: support buffer size is more than one." << std::endl;
      return false;
    }

    assert(model_->buffers.size() == 1);
    size_t pool_size = model_->buffers.at(0).data.size();

    int fd = 0;
    fd = zukou::Util::CreateAnonymousFile(pool_size);

    if (!pool_.Init(fd, pool_size)) return false;

    {
      auto buffer_data = static_cast<char *>(
          mmap(nullptr, pool_size, PROT_WRITE, MAP_SHARED, fd, 0));
      std::memcpy(buffer_data, model_->buffers.at(0).data.data(), pool_size);
      munmap(buffer_data, pool_size);
    }

    if (!vertex_shader_.Init(GL_VERTEX_SHADER, gltf_vertex_shader_source))
      return false;
    if (!texture_fragment_shader_.Init(
            GL_FRAGMENT_SHADER, gltf_texture_fragment_shader_source))
      return false;
    if (!color_fragment_shader_.Init(
            GL_FRAGMENT_SHADER, gltf_color_fragment_shader_source))
      return false;

    program_map_["texture"] = new zukou::GlProgram(&system_);
    if (!program_map_["texture"]->Init()) {
      return false;
    }
    program_map_["texture"]->AttachShader(&vertex_shader_);
    program_map_["texture"]->AttachShader(&texture_fragment_shader_);
    program_map_["texture"]->Link();

    program_map_["color"] = new zukou::GlProgram(&system_);
    if (!program_map_["color"]->Init()) {
      return false;
    }
    program_map_["color"]->AttachShader(&vertex_shader_);
    program_map_["color"]->AttachShader(&color_fragment_shader_);
    program_map_["color"]->Link();

    if (!sampler_.Init()) return false;
    sampler_.Parameteri(GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    sampler_.Parameteri(GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    for (int i = 0; i < (int)model_->bufferViews.size(); ++i) {
      const tinygltf::BufferView &bufferView = model_->bufferViews[i];
      if (bufferView.target == 0) {
        std::cout << "TODO: bufferView.target is zero (unsupported)"
                  << std::endl;
        continue;
      }

      int sparse_accessor = -1;
      for (int a_i = 0; a_i < (int)model_->accessors.size(); ++a_i) {
        const auto &accessor = model_->accessors[a_i];
        if (accessor.bufferView == i) {
          if (accessor.sparse.isSparse) {
            std::cout << "TODO: support sparse_accessor" << std::endl;
            sparse_accessor = a_i;
            break;
          }
        }
      }

      if (sparse_accessor >= 0) {
        std::cout << "TODO: support sparse_accessor" << std::endl;
        continue;
      }

      zukou::Buffer *zBuffer = new zukou::Buffer();
      if (!zBuffer->Init(
              &pool_, bufferView.byteOffset, bufferView.byteLength)) {
        std::cerr << "Failed to initialize vertex buffer" << std::endl;
        return false;
      }

      zukou::GlBuffer *zGlBuffer = new zukou::GlBuffer(&system_);
      gl_vertex_buffer_map_[i] = zGlBuffer;
      if (!gl_vertex_buffer_map_[i]->Init()) {
        std::cerr << "Failed to initialize gl vertex buffer" << std::endl;
        return false;
      }
      gl_vertex_buffer_map_[i]->Data(
          bufferView.target, zBuffer, GL_STATIC_DRAW);
    }

    for (auto texture : model_->textures) {
      JpegTexture *jpeg_texture = new JpegTexture(&system_);

      std::filesystem::path path = parent_dir_;
      auto image = model_->images[texture.source];
      path /= image.uri;

      if (!jpeg_texture->Init() || !jpeg_texture->Load(path.c_str())) {
        std::cerr << "Failed to load jpeg texture" << std::endl;
        return false;
      }
      texture_map_.emplace(texture.source, jpeg_texture);
    }

    return true;
  }

  void RenderScene()
  {
    assert(model_->scenes.size() > 0);

    int scene_to_display = model_->defaultScene > -1 ? model_->defaultScene : 0;
    const tinygltf::Scene &scene = model_->scenes[scene_to_display];
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
      this->RenderNode(model_->nodes[scene.nodes[i]]);
    }
  }

  void RenderNode(const tinygltf::Node &node)
  {
    if (node.matrix.size() == 16) {
      // TODO: row major or column major
      matrix_stack_.push_back(glm::make_mat4(node.matrix.data()));
    } else {
      glm::mat4 mat(1);

      if (node.translation.size() == 3) {
        glm::mat4 T = glm::translate(
            glm::mat4(1), glm::vec3(node.translation[0], node.translation[1],
                              node.translation[2]));
        mat *= T;
      }

      if (node.rotation.size() == 4) {
        glm::mat4 R = glm::toMat4(glm::quat(node.rotation[3], node.rotation[0],
            node.rotation[1], node.rotation[2]));
        mat *= R;
      }

      if (node.scale.size() == 3) {
        glm::mat4 S = glm::scale(glm::mat4(1),
            glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
        mat *= S;
      }

      matrix_stack_.push_back(mat);
    }

    if (node.mesh > -1) {
      assert(node.mesh < (int)model_->meshes.size());
      this->RenderMesh(model_->meshes[node.mesh]);
    }

    for (size_t i = 0; i < node.children.size(); i++) {
      assert(node.children[i] < (int)model_->nodes.size());
      this->RenderNode(model_->nodes[node.children[i]]);
    }

    matrix_stack_.pop_back();
  }

  void RenderMesh(const tinygltf::Mesh &mesh)
  {
    for (size_t i = 0; i < mesh.primitives.size(); ++i) {
      zukou::RenderingUnit *rendering_unit = new zukou::RenderingUnit(&system_);
      zukou::GlBaseTechnique *base_technique =
          new zukou::GlBaseTechnique(&system_);

      if (!rendering_unit->Init(&bounded_)) {
        std::cerr << "Failed to initialize rendering_unit" << std::endl;
        return;
      }

      if (!base_technique->Init(rendering_unit)) {
        std::cerr << "Failed to initialize base_technique" << std::endl;
        return;
      }

      zukou::GlVertexArray *vertex_array = new zukou::GlVertexArray(&system_);
      if (!vertex_array->Init()) {
        std::cerr << "Failed to initialize vertex_array" << std::endl;
        return;
      }

      const tinygltf::Primitive &primitive = mesh.primitives[i];

      tinygltf::Material material = model_->materials[primitive.material];

      int texture_index = material.pbrMetallicRoughness.baseColorTexture.index;
      // TODO: sampler setting
      if (texture_index >= 0) {
        tinygltf::Texture texture = model_->textures[texture_index];
        JpegTexture *jpeg_texture = texture_map_[texture.source];
        jpeg_texture->GenerateMipmap(GL_TEXTURE_2D);
        base_technique->Bind(
            0, "in_texture", jpeg_texture, GL_TEXTURE_2D, &sampler_);

        for (auto [extension, value] :
            material.pbrMetallicRoughness.baseColorTexture.extensions) {
          if (extension == "KHR_texture_transform") {
            glm::vec2 uniform_offset(0.0f, 0.0f);
            if (value.Has("offset")) {
              auto offset = value.Get("offset");
              uniform_offset[0] = offset.Get(0).GetNumberAsDouble();
              uniform_offset[1] = offset.Get(1).GetNumberAsDouble();
            }
            base_technique->Uniform(0, "in_offset", uniform_offset);

            glm::vec2 uniform_scale(1.0f, 1.0f);
            if (value.Has("scale")) {
              auto scale = value.Get("scale");
              uniform_scale[0] = scale.Get(0).GetNumberAsDouble();
              uniform_scale[1] = scale.Get(1).GetNumberAsDouble();
            }
            base_technique->Uniform(0, "in_scale", uniform_scale);

            glm::vec1 uniform_rotation(0.0f);
            if (value.Has("rotation")) {
              auto rotation = value.Get("rotation");
              uniform_rotation[0] = rotation.GetNumberAsDouble();
            }
            base_technique->Uniform(0, "in_rotation", uniform_rotation);
          }
        }

        base_technique->Bind(program_map_["texture"]);
      } else {
        assert(material.pbrMetallicRoughness.baseColorFactor.size() == 4);

        auto base_color = material.pbrMetallicRoughness.baseColorFactor;

        base_technique->Uniform(0, "in_base_color",
            glm::vec4(
                base_color[0], base_color[1], base_color[2], base_color[3]));
        base_technique->Bind(program_map_["color"]);
      }

      for (auto [attribute, index] : primitive.attributes) {
        assert(index >= 0);
        const tinygltf::Accessor &accessor = model_->accessors[index];
        int size = 1;
        if (accessor.type == TINYGLTF_TYPE_SCALAR) {
          size = 1;
        } else if (accessor.type == TINYGLTF_TYPE_VEC2) {
          size = 2;
        } else if (accessor.type == TINYGLTF_TYPE_VEC3) {
          size = 3;
        } else if (accessor.type == TINYGLTF_TYPE_VEC4) {
          size = 4;
        } else {
          assert(0);
        }

        if ((attribute != "POSITION") && (attribute != "NORMAL") &&
            (attribute != "TEXCOORD_0")) {
          std::cerr << "TODO: support attribute name is " << attribute
                    << std::endl;
          continue;
        }

        // TODO: more robust
        int location;
        if (attribute == "POSITION")
          location = 0;
        else if (attribute == "NORMAL")
          location = 1;
        else if (attribute == "TEXCOORD_0")
          location = 2;
        else
          assert(0);

        // compute byteStride from accessor + bufferView.
        int byteStride =
            accessor.ByteStride(model_->bufferViews[accessor.bufferView]);
        assert(byteStride != -1);

        assert(gl_vertex_buffer_map_.count(accessor.bufferView) > 0);
        vertex_array->Enable(location);
        vertex_array->VertexAttribPointer(location, size,
            accessor.componentType, accessor.normalized ? GL_TRUE : GL_FALSE,
            byteStride, accessor.byteOffset,
            gl_vertex_buffer_map_[accessor.bufferView]);
      }
      base_technique->Bind(vertex_array);

      assert(primitive.indices >= 0);
      const tinygltf::Accessor &indexAccessor =
          model_->accessors[primitive.indices];

      int mode = -1;
      switch (primitive.mode) {
        case TINYGLTF_MODE_TRIANGLES:
          mode = GL_TRIANGLES;
          break;
        case TINYGLTF_MODE_TRIANGLE_STRIP:
          mode = GL_TRIANGLE_STRIP;
          break;
        case TINYGLTF_MODE_TRIANGLE_FAN:
          mode = GL_TRIANGLE_FAN;
          break;
        case TINYGLTF_MODE_POINTS:
          mode = GL_POINTS;
          break;
        case TINYGLTF_MODE_LINE:
          mode = GL_LINES;
          break;
        case TINYGLTF_MODE_LINE_LOOP:
          mode = GL_LINE_LOOP;
          break;
        default:
          assert(0);
      }

      if (model_->bufferViews[indexAccessor.bufferView].target !=
          GL_ELEMENT_ARRAY_BUFFER) {
        std::cerr << "TODO: support other than gl_element_array_buffer"
                  << std::endl;
      }

      base_technique->Uniform(0, "local_model", CalculateLocalModel());

      base_technique->DrawElements(mode, indexAccessor.count,
          indexAccessor.componentType, indexAccessor.byteOffset,
          gl_vertex_buffer_map_[indexAccessor.bufferView]);
    }

    bounded_.Commit();
  }

  void SetupRegion(glm::vec3 half_size)
  {
    zukou::Region region(&system_);
    if (!region.Init()) {
      std::cerr << "region init failed!" << std::endl;
      return;
    }

    region.AddCuboid(half_size, glm::vec3(0), glm::quat(glm::vec3(0)));

    bounded_.SetRegion(&region);

    bounded_.Commit();
  }
};

int
main(int argc, char const *argv[])
{
  tinygltf::Model model;
  tinygltf::TinyGLTF loader;
  std::string err;
  std::string warn;

  if (argc != 2) {
    std::cerr << "argument number error" << std::endl;
    std::cerr << "Usage:\t" << argv[0] << " [FILE].gltf" << std::endl;
    return EXIT_FAILURE;
  }

  std::string path(argv[1]);

  bool ret = false;
  // TODO: .glb
  ret = loader.LoadASCIIFromFile(&model, &err, &warn, path.c_str());

  if (!warn.empty()) std::cerr << warn << std::endl;
  if (!err.empty()) {
    std::cerr << err << std::endl;
    return EXIT_FAILURE;
  }

  if (!ret) {
    std::cerr << "Failed to load .glTF : " << path << std::endl;
    return EXIT_FAILURE;
  }

  Viewer viewer(&model, std::filesystem::absolute(path).parent_path());

  glm::vec3 half_size(1.0, 1.0, 1.0);

  if (!viewer.Init(half_size)) {
    std::cerr << "system init error" << std::endl;
    return EXIT_FAILURE;
  }

  return viewer.Run();
}

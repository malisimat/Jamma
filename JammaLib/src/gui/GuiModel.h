#pragma once

#include <atomic>
#include <mutex>

#include "GuiElement.h"
#include "GlUtils.h"

namespace gui
{
	class GuiModelParams : public base::GuiElementParams
	{
	public:
		GuiModelParams() :
			base::GuiElementParams(0, DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{}),
			ModelTextures({}),
			ModelShaders({}),
			Verts({}),
			Uvs({})
		{
		}

		GuiModelParams(base::GuiElementParams params) :
			base::GuiElementParams(params),
			ModelTextures({}),
			ModelShaders({}),
			Verts({}),
			Uvs({})
		{
		}

	public:
		std::vector<std::string> ModelTextures;
		std::vector<std::string> ModelShaders;
		std::vector<float> Verts;
		std::vector<float> Uvs;
	};

	class GuiModel :
		public base::GuiElement
	{
	public:
		struct InstanceAttribute
		{
			unsigned int AttributeIndex;
			unsigned int ComponentCount;
			std::vector<float> Data;
		};

		GuiModel(GuiModelParams params);

	public:
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;

		void SetGeometry(std::vector<float> coords, std::vector<float> uvs);
		void SetInstanceAttributes(std::vector<InstanceAttribute> attributes, unsigned int instanceCount);
		unsigned int InstanceCount() const noexcept { return _instanceCount; }

	protected:
		static bool HasCurrentGlContext() noexcept;
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;
		bool SyncInstanceAttributes();
		static bool HasSameInstanceAttributeLayout(const std::vector<InstanceAttribute>& lhs,
			const std::vector<InstanceAttribute>& rhs);

		bool InitTextures(resources::ResourceLib& resourceLib);
		bool InitShaders(resources::ResourceLib& resourceLib);
		bool InitVertexArray(std::vector<float> verts, std::vector<float> uvs);
		bool InitInstanceAttributes();

		// Thread-safe locked accessors — always use these instead of _modelTextures/_modelShaders
		// directly. They acquire _modelStateMutex so reads can never race against Init writes.
		virtual std::weak_ptr<resources::TextureResource> GetTexture();
		virtual std::weak_ptr<resources::ShaderResource> GetShader();

		// Index-based variants for derived classes that need a specific entry
		// (e.g. multi-shader models that swap by render pass).  Protected so
		// only this class and its subclasses can call them — external code must
		// go through GetTexture()/GetShader().
		std::weak_ptr<resources::TextureResource> GetTextureAt(unsigned int index);
		std::weak_ptr<resources::ShaderResource>  GetShaderAt(unsigned int index);

	protected:
		bool _resourcesInitialised;
		std::atomic<bool> _geometryNeedsUpdating;
		std::atomic<bool> _instanceAttributesNeedUpdating;
		bool _usesInstanceAttributes;
		GuiModelParams _modelParams;
		std::vector<float> _backVerts;
		std::vector<float> _backUvs;
		std::vector<InstanceAttribute> _backInstanceAttributes;
		std::vector<InstanceAttribute> _instanceAttributes;
		GLuint _vertexArray;
		GLuint _vertexBuffer[3];
		std::vector<GLuint> _instanceBuffers;
		unsigned int _numTris;
		unsigned int _backInstanceCount;
		unsigned int _instanceCount;
		mutable std::mutex _modelStateMutex;

	private:
		// Private: all access must go through the locked Get*() helpers above.
		// Keeping these private means the compiler will reject any future attempt
		// to read _modelTextures/_modelShaders from a derived class without locking.
		std::vector<std::weak_ptr<resources::TextureResource>> _modelTextures;
		std::vector<std::weak_ptr<resources::ShaderResource>> _modelShaders;
	};
}

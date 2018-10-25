#ifndef _ZMQ_MESSAGE_H_
#define _ZMQ_MESSAGE_H_

#include "zmq.hpp"
#include "base_types.h"
#include "zmq_serializer.hpp"
#include "zmq_deserializer.hpp"


class VRayMessage {
public:
	enum class Type : char {
		None,
		Image,
		ChangePlugin,
		ChangeRenderer,
		VRayLog,
	};

	enum class PluginAction : char {
		None,
		Create,
		Remove,
		Update,
		Replace
	};

	enum class RendererAction : char {
		None,
		Free,
		Start,
		Stop,
		Pause,
		Resume,
		Resize,
		Reset,
		_ArgumentRenderAction,
		Init,
		ResetsHosts,
		LoadScene,
		AppendScene,
		ExportScene,
		SetRenderMode,
		SetAnimationProperties,
		SetCurrentTime,
		SetCurrentFrame,
		ClearFrameValues,
		SetRendererState,
		GetImage,
		SetQuality,
		SetCurrentCamera,
		SetCommitAction,
		SetVfbShow,
		SetViewportImageFormat,
		SetRenderRegion,
		SetCropRegion,
	};

	enum class DRFlags : char {
		None              = 0,
		EnableDr          = 1 << 1,
		RenderOnlyOnHosts = 1 << 2,
		_SerializationShift = 8,
	};

	enum class RendererType : char {
		None,
		RT,
		Animation,
		SingleFrame,
		Preview,
		_SerializationShift = 0,
	};

	enum class ValueSetter : char {
		None,
		Default,
		AsString
	};

	enum class RendererState : char {
		None,
		Abort,
		Continue,
		Progress,
		ProgressMessage,
	};

	VRayMessage()
	    : type(Type::None)
	    , rendererAction(RendererAction::None)
	    , rendererType(RendererType::None)
	    , drFlags(DRFlags::None)
	    , rendererState(RendererState::None)
	    , valueSetter(ValueSetter::None)
	    , pluginAction(PluginAction::None)
	{}

	VRayMessage(VRayMessage && other)
	    : message(0)
	    , type(other.type)
	    , rendererAction(other.rendererAction)
	    , rendererType(other.rendererType)
	    , drFlags(other.drFlags)
	    , rendererState(other.rendererState)
	    , valueSetter(other.valueSetter)
	    , pluginAction(other.pluginAction)
	    , pluginName(std::move(other.pluginName))
	    , pluginType(std::move(other.pluginType))
	    , pluginProperty(std::move(other.pluginProperty))
	    , logLevel(other.logLevel)
	    , rendererWidth(other.rendererWidth)
	    , rendererHeight(other.rendererHeight)
	    , value(std::move(other.value))
	{
		this->message.move(&other.message);
	}

	/// Create message from data, usually to be sent
	explicit VRayMessage(const char * data, int size)
	    : message(data, size)
	    , rendererAction(RendererAction::None)
	    , rendererType(RendererType::None)
	    , drFlags(DRFlags::None)
	    , rendererState(RendererState::None)
	    , valueSetter(ValueSetter::None)
	    , pluginAction(PluginAction::None)
	{}

	/// Create VRayMessage from zmq::message_t parsing the data
	static VRayMessage fromZmqMessage(zmq::message_t & message) {
		VRayMessage msg;
		msg.message.move(&message);
		msg.parse();
		return msg;
	}

	static zmq::message_t fromData(const char * data, int size) {
		return zmq::message_t(data, size);
	}

	zmq::message_t & getInternalMessage() {
		return this->message;
	}

	const std::string getPluginNew() const {
		if (pluginAction == PluginAction::Replace && type == Type::ChangePlugin) {
			return value.as<VRayBaseTypes::AttrSimpleType<std::string>>();
		} else {
			assert((pluginAction == PluginAction::Replace && type == Type::ChangePlugin) && "Getting plugin new");
			return "";
		}
	}

	/// Get plugin property
	const std::string & getProperty() const {
		return pluginProperty;
	}

	/// Get the plugin instance id
	const std::string & getPlugin() const {
		return pluginName;
	}

	/// Get the plugin type name
	const std::string & getPluginType() const {
		return pluginType;
	}

	/// Get the message type
	Type getType() const {
		return type;
	}

	/// If type == ChangePlugin then get the plugin action
	PluginAction getPluginAction() const {
		return pluginAction;
	}

	/// If type == ChangeRenderer then get the renderer action
	RendererAction getRendererAction() const {
		return rendererAction;
	}

	/// If PluginAction will update plugin property check if it should be set as string
	ValueSetter getValueSetter() const {
		return valueSetter;
	}

	/// Get renderer type if renderer action is init
	RendererType getRendererType() const {
		return rendererType;
	}

	/// Get renderer statate for renderer action set renderer state
	RendererState getRendererState() const {
		return rendererState;
	}

	/// Get dr flags for renderer init renderer action
	DRFlags getDrFlags() const {
		return drFlags;
	}

	/// Get logLevel for VRay log messages
	int getLogLevel() const {
		return logLevel;
	}

	/// Get the renderer size for renderer action resize
	void getRendererSize(int & width, int & height) const {
		width = rendererWidth;
		height = rendererHeight;
	}

	/// If message is update plugin param, get pointer to the internal param value
	template <typename T>
	const T * getValue() const {
		return value.asPtr<T>();
	}

	/// If message is update plugin param get the attr value object that stores the param value
	const VRayBaseTypes::AttrValue & getAttrValue() const {
		return value;
	}

	/// If message is update plugin param, get the value type
	VRayBaseTypes::ValueType getValueType() const {
		return value.type;
	}

	/// Static methods for creating messages
	///
	static zmq::message_t msgPluginCreate(const std::string & pluginName, const std::string & pluginType) {
		SerializerStream strm;
		strm << VRayMessage::Type::ChangePlugin << pluginName << PluginAction::Create << pluginType;
		return fromStream(strm);
	}

	static zmq::message_t msgPluginReplace(const std::string & pluginOld, const std::string & pluginNew) {
		VRayBaseTypes::AttrSimpleType<std::string> valWrapper(pluginNew);
		SerializerStream strm;
		strm << VRayMessage::Type::ChangePlugin << pluginOld << PluginAction::Replace << valWrapper.getType() << valWrapper;
		return fromStream(strm);
	}

	static zmq::message_t msgPluginAction(const std::string & plugin, PluginAction action) {
		assert((action == PluginAction::Create || action == PluginAction::Remove) && "Wrong PluginAction");
		SerializerStream strm;
		strm << VRayMessage::Type::ChangePlugin << plugin << action;
		return fromStream(strm);
	}

	/// Creates message to control a plugin property
	template <typename T>
	static zmq::message_t msgPluginSetProperty(const std::string & plugin, const std::string & property, const T & value) {
		using namespace std;
		SerializerStream strm;
		strm << VRayMessage::Type::ChangePlugin << plugin << PluginAction::Update << property << ValueSetter::Default << value.getType() << value;
		return fromStream(strm);
	}

	static zmq::message_t msgPluginSetProperty(const std::string & plugin, const std::string & property, const VRayBaseTypes::AttrValue & value) {
		using namespace std;
		SerializerStream strm;
		strm << VRayMessage::Type::ChangePlugin << plugin << PluginAction::Update << property << ValueSetter::Default << value;
		return fromStream(strm);
	}

	static zmq::message_t msgPluginSetPropertyString(const std::string & plugin, const std::string & property, const std::string & value) {
		using namespace std;
		SerializerStream strm;
		strm << VRayMessage::Type::ChangePlugin << plugin << PluginAction::Update << property
		     << ValueSetter::AsString << VRayBaseTypes::ValueType::ValueTypeString << value;
		return fromStream(strm);
	}

	static zmq::message_t msgImageSet(const VRayBaseTypes::AttrImageSet & value) {
		SerializerStream strm;
		strm << VRayMessage::Type::Image << value.getType() << value;
		return fromStream(strm);
	}

	static zmq::message_t msgVRayLog(int level, const std::string & log) {
		SerializerStream strm;
		VRayBaseTypes::AttrSimpleType<std::string> val(log);
		strm << VRayMessage::Type::VRayLog << level << val.getType() << log;
		return fromStream(strm);
	}

	/// Create message to control renderer
	static zmq::message_t msgRendererAction(RendererAction action) {
		assert(action < RendererAction::_ArgumentRenderAction && "Renderer action provided requires argument!");
		SerializerStream strm;
		strm << Type::ChangeRenderer << action;
		return fromStream(strm);
	}

	template <typename T>
	static zmq::message_t msgRendererAction(RendererAction action, const T & value) {
		assert(action > RendererAction::_ArgumentRenderAction && "Renderer action provided requires NO argument!");
		SerializerStream strm;
		VRayBaseTypes::AttrSimpleType<T> valWrapper(value);
		strm << Type::ChangeRenderer << action << valWrapper.getType() << valWrapper;
		return fromStream(strm);
	}

	static zmq::message_t msgRendererActionInit(RendererType type, DRFlags drFlags) {
		SerializerStream strm;
		const int value = static_cast<int>(drFlags) << static_cast<int>(DRFlags::_SerializationShift)
		                | static_cast<int>(type) << static_cast<int>(RendererType::_SerializationShift);
		return msgRendererAction(RendererAction::Init, value);
	}

	static zmq::message_t msgRendererAction(RendererAction action, const VRayBaseTypes::AttrListInt & value) {
		assert(action > RendererAction::_ArgumentRenderAction && "Renderer action provided requires NO argument!");
		SerializerStream strm;
		strm << Type::ChangeRenderer << action << value.getType() << value;
		return fromStream(strm);
	}

	template <typename T>
	static zmq::message_t msgRendererState(RendererState state, const T & val) {
		VRayBaseTypes::AttrSimpleType<T> valWrapper(val);
		SerializerStream strm;
		strm << Type::ChangeRenderer << RendererAction::SetRendererState << state << valWrapper.getType() << valWrapper;
		return fromStream(strm);
	}

	static zmq::message_t msgRendererResize(int width, int height) {
		SerializerStream strm;
		strm << Type::ChangeRenderer << RendererAction::Resize << width << height;
		return fromStream(strm);
	}

private:
	static zmq::message_t fromStream(SerializerStream & strm) {
		return fromData(strm.getData(), strm.getSize());
	}

	void parse() {
		using namespace VRayBaseTypes;

		DeserializerStream stream(reinterpret_cast<char*>(message.data()), message.size());
		stream >> type;

		if (type == Type::ChangePlugin) {
			stream >> pluginName >> pluginAction;
			if (pluginAction == PluginAction::Update) {
				stream >> pluginProperty >> valueSetter >> value;
			} else if (pluginAction == PluginAction::Create) {
				if (stream.hasMore()) {
					stream >> pluginType;
				}
			} else if (pluginAction == PluginAction::Replace) {
				assert(stream.hasMore() && "Missing new plugin for replace plugin");
				stream >> value;
			}
		} else if (type == Type::Image) {
			stream >> value;
		} else if (type == Type::VRayLog) {
			stream >> logLevel >> value;
			assert(value.type == VRayBaseTypes::ValueTypeString && "Type::VRayLog must be a string value");
		} else if (type == Type::ChangeRenderer) {
			stream >> rendererAction;
			if (rendererAction == RendererAction::Resize) {
				stream >> rendererWidth >> rendererHeight;
			} else if (rendererAction == RendererAction::Init) {
				stream >> value;
				const int val = value.as<AttrSimpleType<int>>().value;
				drFlags = static_cast<DRFlags>((val >> static_cast<int>(DRFlags::_SerializationShift)) & 0xff);
				rendererType = static_cast<RendererType>((val >> static_cast<int>(RendererType::_SerializationShift)) & 0xff);
			} else if (rendererAction == RendererAction::SetRendererState) {
				stream >> rendererState >> value;
			} else if (rendererAction > RendererAction::_ArgumentRenderAction) {
				stream >> value;
			}
		}
	}


	zmq::message_t            message;
	Type                      type;

	RendererAction            rendererAction;
	RendererType              rendererType;
	DRFlags                   drFlags;
	RendererState             rendererState;

	ValueSetter               valueSetter;

	PluginAction              pluginAction;
	std::string               pluginName;
	std::string               pluginType;
	std::string               pluginProperty;

	int                       logLevel;
	int                       rendererWidth;
	int                       rendererHeight;

	VRayBaseTypes::AttrValue  value;
private:
	VRayMessage(const VRayMessage&) = delete;
	VRayMessage& operator=(const VRayMessage&) = delete;
};

#endif // _ZMQ_MESSAGE_H_

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
		Init,
		Free,
		Start,
		Stop,
		Pause,
		Resume,
		Resize,
		SetOnBucketReady,
		_ArgumentRenderAction,
		AddHosts,
		RemoveHosts,
		LoadScene,
		AppendScene,
		ExportScene,
		SetRenderMode,
		SetAnimationProperties,
		SetCurrentTime,
		SetCurrentFrame,
		ClearFrameValues,
		SetRendererState,
		SetRendererType,
		GetImage,
		SetQuality,
		SetCurrentCamera,
		SetCommitAction,
		SetVfbShow,
		SetViewportImageFormat,
	};

	enum class ValueSetter : char {
		None,
		Default,
		AsString
	};

	enum class RendererType : char {
		None,
		RT,
		Animation,
		SingleFrame,
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
	    , rendererState(RendererState::None)
	    , valueSetter(ValueSetter::None)
	    , pluginAction(PluginAction::None)
	{}

	VRayMessage(VRayMessage && other)
	    : message(0)
	    , type(other.type)
	    , rendererAction(other.rendererAction)
	    , rendererType(other.rendererType)
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

	static VRayMessage fromData(const char * data, int size) {
		return VRayMessage(data, size);
	}

	zmq::message_t & getMessage() {
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

	const std::string & getProperty() const {
		return this->pluginProperty;
	}

	const std::string & getPlugin() const {
		return this->pluginName;
	}

	const std::string & getPluginType() const {
		return this->pluginType;
	}

	Type getType() const {
		return type;
	}

	PluginAction getPluginAction() const {
		return pluginAction;
	}

	RendererAction getRendererAction() const {
		return rendererAction;
	}

	ValueSetter getValueSetter() const {
		return valueSetter;
	}

	RendererType getRendererType() const {
		return rendererType;
	}

	RendererState getRendererState() const {
		return rendererState;
	}

	int getLogLevel() const {
		return logLevel;
	}

	void getRendererSize(int & width, int & height) const {
		width = this->rendererWidth;
		height = this->rendererHeight;
	}

	template <typename T>
	const T * getValue() const {
		return value.asPtr<T>();
	}

	const VRayBaseTypes::AttrValue & getAttrValue() const {
		return value;
	}

	VRayBaseTypes::ValueType getValueType() const {
		return value.type;
	}

	/// Static methods for creating messages
	///
	static VRayMessage msgPluginCreate(const std::string & pluginName, const std::string & pluginType) {
		SerializerStream strm;
		strm << VRayMessage::Type::ChangePlugin << pluginName << PluginAction::Create << pluginType;
		return fromStream(strm);
	}

	static VRayMessage msgPluginReplace(const std::string & pluginOld, const std::string & pluginNew) {
		VRayBaseTypes::AttrSimpleType<std::string> valWrapper(pluginNew);
		SerializerStream strm;
		strm << VRayMessage::Type::ChangePlugin << pluginOld << PluginAction::Replace << valWrapper.getType() << valWrapper;
		return fromStream(strm);
	}

	static VRayMessage msgPluginAction(const std::string & plugin, PluginAction action) {
		assert((action == PluginAction::Create || action == PluginAction::Remove) && "Wrong PluginAction");
		SerializerStream strm;
		strm << VRayMessage::Type::ChangePlugin << plugin << action;
		return fromStream(strm);
	}

	/// Creates message to control a plugin property
	template <typename T>
	static VRayMessage msgPluginSetProperty(const std::string & plugin, const std::string & property, const T & value) {
		using namespace std;
		SerializerStream strm;
		strm << VRayMessage::Type::ChangePlugin << plugin << PluginAction::Update << property << ValueSetter::Default << value.getType() << value;
		return fromStream(strm);
	}

	static VRayMessage msgPluginSetProperty(const std::string & plugin, const std::string & property, const VRayBaseTypes::AttrValue & value) {
		using namespace std;
		SerializerStream strm;
		strm << VRayMessage::Type::ChangePlugin << plugin << PluginAction::Update << property << ValueSetter::Default << value;
		return fromStream(strm);
	}

	static VRayMessage msgPluginSetPropertyString(const std::string & plugin, const std::string & property, const std::string & value) {
		using namespace std;
		SerializerStream strm;
		strm << VRayMessage::Type::ChangePlugin << plugin << PluginAction::Update << property
		     << ValueSetter::AsString << VRayBaseTypes::ValueType::ValueTypeString << value;
		return fromStream(strm);
	}

	static VRayMessage msgImageSet(const VRayBaseTypes::AttrImageSet & value) {
		SerializerStream strm;
		strm << VRayMessage::Type::Image << value.getType() << value;
		return fromStream(strm);
	}

	static VRayMessage msgVRayLog(int level,const std::string & log) {
		SerializerStream strm;
		VRayBaseTypes::AttrSimpleType<std::string> val(log);
		strm << VRayMessage::Type::VRayLog << level << val.getType() << log;
		return fromStream(strm);
	}

	/// Create message to control renderer
	static VRayMessage msgRendererAction(const RendererAction & action) {
		assert(action < RendererAction::_ArgumentRenderAction && "Renderer action provided requires argument!");
		SerializerStream strm;
		strm << Type::ChangeRenderer << action;
		return fromStream(strm);
	}

	template <typename T>
	static VRayMessage msgRendererAction(const RendererAction & action, const T & value) {
		assert(action > RendererAction::_ArgumentRenderAction && "Renderer action provided requires NO argument!");
		SerializerStream strm;
		VRayBaseTypes::AttrSimpleType<T> valWrapper(value);
		strm << Type::ChangeRenderer << action << valWrapper.getType() << valWrapper;
		return fromStream(strm);
	}

	template <typename T>
	static VRayMessage msgRendererState(RendererState state, const T & val) {
		VRayBaseTypes::AttrSimpleType<T> valWrapper(val);
		SerializerStream strm;
		strm << Type::ChangeRenderer << RendererAction::SetRendererState << state << valWrapper.getType() << valWrapper;
		return fromStream(strm);
	}

	static VRayMessage msgRendererType(RendererType type) {
		SerializerStream strm;
		strm << Type::ChangeRenderer << RendererAction::SetRendererType << type;
		return fromStream(strm);
	}

	static VRayMessage msgRendererResize(int width, int height) {
		SerializerStream strm;
		strm << Type::ChangeRenderer << RendererAction::Resize << width << height;
		return fromStream(strm);
	}

private:
	template <typename T>
	T * setValue() {
		new(value.asPtr<T>)T();
		return value.asPtr<T>;
	}

	static VRayMessage fromStream(SerializerStream & strm) {
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
			} else if (rendererAction == RendererAction::SetRendererType) {
				stream >> rendererType;
			} else if (rendererAction == RendererAction::SetRendererState) {
				stream >> rendererState >> value;
			} else if (rendererAction > RendererAction::_ArgumentRenderAction) {
				stream >> value;
			}
		}
	}

	char * data() {
		return reinterpret_cast<char*>(this->message.data());
	}

	zmq::message_t            message;
	Type                      type;

	RendererAction            rendererAction;
	RendererType              rendererType;
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

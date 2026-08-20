// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#pragma once

#include <Nodos/Plugin.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace nos::audio
{
// One node status message per kind of problem, so each can be raised and cleared
// on its own. Nodes report through this rather than returning failure: a node
// that fails ends the path it runs on, and everything the path had left to do,
// signalling the GPU among it, is then never done.
template <typename ErrorType>
struct NodeErrors
{
	NodeErrors(NodeContext& context) : Context(context) {}

	void Set(ErrorType errorType, fb::NodeStatusMessageType msgType, std::string text, std::string details = "")
	{
		auto it = Messages.find(errorType);
		if (it != Messages.end() && it->second.text == text && it->second.type == msgType && it->second.details == details)
			return;
		Messages[errorType] = fb::TNodeStatusMessage{{}, std::move(text), msgType, std::move(details), 0, true, false};
		Send();
	}

	void Clear(ErrorType errorType)
	{
		if (Messages.erase(errorType) == 0)
			return;
		Send();
	}

private:
	// Only called when the set of messages actually changed, because nodes report
	// from ExecuteNode and would otherwise send one of these every frame.
	void Send()
	{
		if (Messages.empty())
		{
			Context.ClearNodeStatusMessages();
			return;
		}
		std::vector<fb::TNodeStatusMessage> messages;
		messages.reserve(Messages.size());
		for (auto& [type, message] : Messages)
			messages.push_back(message);
		Context.SetNodeStatusMessages(messages);
	}

	NodeContext& Context;
	std::unordered_map<ErrorType, fb::TNodeStatusMessage> Messages;
};
}

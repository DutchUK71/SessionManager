#include "PdfTemplateResolver.h"

#include "Rules/Entity.h"
#include "Rules/Instance.h"
#include "Rules/Relationship.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace
{
	constexpr int MaximumRegionDepth = 16;
	constexpr size_t MaximumBlocks = 2000;
	constexpr size_t MaximumRuns = 10000;
	constexpr size_t MaximumTextCharacters = 1000000;

	struct ScopeEntry
	{
		std::string regionId;
		std::string entityId;
		std::shared_ptr<Instance> instance;
	};

	struct ResolveState
	{
		const RuleSet& ruleSet;
		PdfTemplateResolver::Result& result;
		std::unordered_set<std::string> visitedBlockPaths;
		std::unordered_set<std::string> visitedRunPaths;
		std::unordered_map<std::string, std::string> blockIdPaths;
		std::unordered_map<std::string, std::string> runIdPaths;
		size_t blockCount = 0;
		size_t runCount = 0;
		size_t textCharacters = 0;
	};

	std::string StringValue(const nlohmann::json& value, const char* key)
	{
		if(value.is_object() && value.contains(key) && value.at(key).is_string())
		{
			return value.at(key).get<std::string>();
		}

		return "";
	}

	double NumberValue(const nlohmann::json& value, const char* key, double fallback)
	{
		return value.is_object() && value.contains(key) && value.at(key).is_number()
			? value.at(key).get<double>()
			: fallback;
	}

	void AddDiagnostic(PdfTemplateResolver::Result& result,
	                   const std::string& code,
	                   const std::string& message,
	                   const std::string& path)
	{
		result.diagnostics.push_back({code, message, path});
	}

	std::shared_ptr<Entity> FindEntity(const RuleSet& ruleSet, const std::string& entityId)
	{
		if(ruleSet.entities == nullptr)
		{
			return nullptr;
		}

		for(const std::shared_ptr<Entity>& entity : *ruleSet.entities)
		{
			if(entity != nullptr && entity->entityid == entityId)
			{
				return entity;
			}
		}

		return nullptr;
	}

	std::shared_ptr<Relationship> FindDirectChildRelationship(const std::shared_ptr<Entity>& parent,
	                                                         const std::string& relationshipId,
	                                                         const std::string& childEntityId)
	{
		if(parent == nullptr)
		{
			return nullptr;
		}

		for(const std::shared_ptr<Relationship>& relationship : parent->relationships)
		{
			if(relationship != nullptr
				&& relationship->id == relationshipId
				&& relationship->sourceEntity == parent->entityid
				&& relationship->targetEntity == childEntityId
				&& (relationship->type == "1M" || relationship->type == "11"))
			{
				return relationship;
			}
		}

		return nullptr;
	}

	std::vector<std::shared_ptr<Instance>> RelatedInstances(const std::shared_ptr<Instance>& parent,
	                                                       const std::string& relationshipId,
	                                                       const std::string& childEntityId)
	{
		std::vector<std::shared_ptr<Instance>> result;
		if(parent == nullptr)
		{
			return result;
		}

		for(size_t index : parent->FindRelationshipLinkIndexesByRelationshipId(relationshipId))
		{
			if(index >= parent->relationshipLinks.size())
			{
				continue;
			}

			const InstanceRelationshipLink& link = parent->relationshipLinks[index];
			std::shared_ptr<Instance> child = link.linkedInstance.lock();
			if(link.outgoing
				&& link.sourceInstanceId == parent->instanceId
				&& link.targetEntityId == childEntityId
				&& child != nullptr
				&& child->entityId == childEntityId)
			{
				result.push_back(child);
			}
		}

		return result;
	}

	bool IsHexColour(const std::string& value)
	{
		if(value.size() != 7 || value[0] != '#')
		{
			return false;
		}

		return std::all_of(value.begin() + 1, value.end(), [](unsigned char ch) {
			return std::isxdigit(ch) != 0;
		});
	}

	bool IsFont(const std::string& value)
	{
		return value == "Noto Sans" || value == "Noto Serif" || value == "Noto Sans Mono";
	}

	bool CopyTextStyle(const nlohmann::json& source,
	                  nlohmann::json& target,
	                  PdfTemplateResolver::Result& result,
	                  const std::string& path,
	                  bool blockStyle)
	{
		if(!source.is_object())
		{
			AddDiagnostic(result, "pdf_style_invalid", "PDF style must be an object.", path);
			return false;
		}

		target = nlohmann::json::object();
		auto copyString = [&](const char* key) {
			if(source.contains(key) && source.at(key).is_string())
			{
				target[key] = source.at(key);
			}
		};
		auto copyBool = [&](const char* key) {
			if(source.contains(key) && source.at(key).is_boolean())
			{
				target[key] = source.at(key);
			}
		};
		auto copyNumber = [&](const char* key) {
			if(source.contains(key) && source.at(key).is_number())
			{
				target[key] = source.at(key);
			}
		};

		copyString("fontFamily");
		copyNumber("fontSizePoints");
		copyString("textColor");
		copyBool("bold");
		copyBool("italic");
		copyBool("underline");
		if(blockStyle)
		{
			copyString("alignment");
			copyNumber("spaceBeforePoints");
			copyNumber("spaceAfterPoints");
		}

		if(target.contains("fontFamily") && !IsFont(target.at("fontFamily").get<std::string>()))
		{
			AddDiagnostic(result, "pdf_font_unsupported", "PDF font is not in the bundled font set.", path + ".fontFamily");
		}
		if(target.contains("fontSizePoints"))
		{
			const double size = target.at("fontSizePoints").get<double>();
			if(size < 6 || size > 72)
			{
				AddDiagnostic(result, "pdf_font_size_invalid", "PDF font size must be between 6 and 72 points.", path + ".fontSizePoints");
			}
		}
		if(target.contains("textColor") && !IsHexColour(target.at("textColor").get<std::string>()))
		{
			AddDiagnostic(result, "pdf_colour_invalid", "PDF text colour must use #RRGGBB format.", path + ".textColor");
		}
		if(blockStyle && target.contains("alignment"))
		{
			const std::string alignment = target.at("alignment").get<std::string>();
			if(alignment != "left" && alignment != "centre" && alignment != "right" && alignment != "justify")
			{
				AddDiagnostic(result, "pdf_alignment_invalid", "Unsupported PDF paragraph alignment.", path + ".alignment");
			}
		}

		return result.diagnostics.empty();
	}

	const ScopeEntry* FindScope(const std::vector<ScopeEntry>& scope, const std::string& regionId)
	{
		for(auto it = scope.rbegin(); it != scope.rend(); ++it)
		{
			if(it->regionId == regionId)
			{
				return &*it;
			}
		}

		return nullptr;
	}

	bool ResolveRuns(const nlohmann::json& runs,
	                 const std::vector<ScopeEntry>& scope,
	                 nlohmann::json& output,
	                 ResolveState& state,
	                 const std::string& path,
	                 bool htmlContent)
	{
		if(!runs.is_array())
		{
			AddDiagnostic(state.result, "pdf_runs_invalid", "PDF runs must be an array.", path);
			return false;
		}

		output = nlohmann::json::array();
		for(size_t index = 0; index < runs.size(); ++index)
		{
			const nlohmann::json& run = runs[index];
			const std::string runPath = path + "[" + std::to_string(index) + "]";
			state.runCount++;
			if(state.runCount > MaximumRuns)
			{
				AddDiagnostic(state.result, "pdf_run_limit", "PDF template exceeds the maximum text run count.", runPath);
				return false;
			}
			if(!run.is_object())
			{
				AddDiagnostic(state.result, "pdf_run_invalid", "PDF run must be an object.", runPath);
				continue;
			}

			const std::string runId = StringValue(run, "id");
			if(state.visitedRunPaths.insert(runPath).second)
			{
				if(runId.empty())
				{
					AddDiagnostic(state.result, "pdf_run_id_invalid", "PDF run ID is missing.", runPath + ".id");
				}
				else
				{
					auto inserted = state.runIdPaths.emplace(runId, runPath);
					if(!inserted.second && inserted.first->second != runPath)
					{
						AddDiagnostic(state.result, "pdf_run_id_invalid", "PDF run ID is duplicated.", runPath + ".id");
					}
				}
			}

			const std::string runType = StringValue(run, "runType");
			std::string text;
			if(runType == "text")
			{
				if(run.contains("text") && run.at("text").is_string())
				{
					text = run.at("text").get<std::string>();
				}
				else
				{
					AddDiagnostic(state.result, "pdf_text_invalid", "PDF text run must contain string text.", runPath + ".text");
				}
			}
			else if(runType == "attribute")
			{
				const std::string sourceRegionId = StringValue(run, "sourceRegionId");
				const std::string entityId = StringValue(run, "entityId");
				const std::string attributeId = StringValue(run, "attributeId");
				const ScopeEntry* source = FindScope(scope, sourceRegionId);
				if(source == nullptr || source->entityId != entityId || source->instance == nullptr)
				{
					AddDiagnostic(state.result, "pdf_attribute_scope_invalid", "PDF attribute reference is outside the current region scope.", runPath);
				}
				else
				{
					std::shared_ptr<Attribute> attribute = source->instance->GetAttributeByID(attributeId);
					if(attribute == nullptr)
					{
						AddDiagnostic(state.result, "pdf_attribute_missing", "PDF attribute reference does not resolve.", runPath + ".attributeId");
					}
					else if(attribute->IsKnown())
					{
						text = attribute->GetStringValue();
					}
				}
			}
			else
			{
				AddDiagnostic(state.result, "pdf_run_type_invalid", "Unsupported PDF run type.", runPath + ".runType");
			}

			state.textCharacters += text.size();
			if(state.textCharacters > MaximumTextCharacters)
			{
				AddDiagnostic(state.result, "pdf_text_limit", "Resolved PDF text exceeds the maximum size.", runPath);
				return false;
			}

			const std::string resolvedRunType = htmlContent && runType == "text" ? "html" : "text";
			nlohmann::json resolvedRun = {{"runType", resolvedRunType}, {"text", text}};
			if(run.contains("style"))
			{
				nlohmann::json style;
				CopyTextStyle(run.at("style"), style, state.result, runPath + ".style", false);
				if(!style.empty())
				{
					resolvedRun["style"] = style;
				}
			}
			output.push_back(resolvedRun);
		}

		return state.result.diagnostics.empty();
	}

	bool ResolveBlocks(const nlohmann::json& blocks,
	                   std::vector<ScopeEntry>& scope,
	                   nlohmann::json& output,
	                   ResolveState& state,
	                   int depth,
	                   const std::string& path)
	{
		if(!blocks.is_array())
		{
			AddDiagnostic(state.result, "pdf_blocks_invalid", "PDF blocks must be an array.", path);
			return false;
		}

		if(!output.is_array())
		{
			output = nlohmann::json::array();
		}

		for(size_t index = 0; index < blocks.size(); ++index)
		{
			const nlohmann::json& block = blocks[index];
			const std::string blockPath = path + "[" + std::to_string(index) + "]";
			state.blockCount++;
			if(state.blockCount > MaximumBlocks)
			{
				AddDiagnostic(state.result, "pdf_block_limit", "PDF template exceeds the maximum block count.", blockPath);
				return false;
			}
			if(!block.is_object())
			{
				AddDiagnostic(state.result, "pdf_block_invalid", "PDF block must be an object.", blockPath);
				continue;
			}

			const std::string blockId = StringValue(block, "id");
			if(state.visitedBlockPaths.insert(blockPath).second)
			{
				if(blockId.empty())
				{
					AddDiagnostic(state.result, "pdf_block_id_invalid", "PDF block ID is missing.", blockPath + ".id");
				}
				else
				{
					auto inserted = state.blockIdPaths.emplace(blockId, blockPath);
					if(!inserted.second && inserted.first->second != blockPath)
					{
						AddDiagnostic(state.result, "pdf_block_id_invalid", "PDF block ID is duplicated.", blockPath + ".id");
					}
				}
			}

			const std::string blockType = StringValue(block, "blockType");
			if(blockType == "region")
			{
				if(depth >= MaximumRegionDepth)
				{
					AddDiagnostic(state.result, "pdf_region_depth", "PDF region nesting exceeds the maximum depth.", blockPath);
					continue;
				}
				if(block.contains("sort") && !block.at("sort").is_null())
				{
					AddDiagnostic(state.result, "pdf_region_sort_unsupported", "Configurable PDF region sorting is not supported in version one.", blockPath + ".sort");
				}

				const std::string relationshipId = StringValue(block, "relationshipId");
				const std::string entityId = StringValue(block, "entityId");
				const ScopeEntry& parentScope = scope.back();
				std::shared_ptr<Entity> parentEntity = FindEntity(state.ruleSet, parentScope.entityId);
				std::shared_ptr<Relationship> relationship = FindDirectChildRelationship(parentEntity, relationshipId, entityId);
				if(relationship == nullptr)
				{
					AddDiagnostic(state.result, "pdf_region_relationship_invalid", "PDF region must follow a direct one-to-one or one-to-many child relationship.", blockPath);
					continue;
				}
				if(std::any_of(scope.begin(), scope.end(), [&](const ScopeEntry& entry) { return entry.entityId == entityId; }))
				{
					AddDiagnostic(state.result, "pdf_region_cycle", "PDF region nesting cannot contain an entity cycle.", blockPath);
					continue;
				}

				const nlohmann::json childBlocks = block.contains("blocks") ? block.at("blocks") : nlohmann::json::array();
				const std::vector<std::shared_ptr<Instance>> children = RelatedInstances(parentScope.instance, relationshipId, entityId);
				if(relationship->type == "11" && children.size() > 1)
				{
					AddDiagnostic(state.result, "pdf_region_cardinality_violation", "A one-to-one PDF region resolved to more than one linked instance.", blockPath);
					continue;
				}

				for(const std::shared_ptr<Instance>& child : children)
				{
					scope.push_back({blockId, entityId, child});
					ResolveBlocks(childBlocks, scope, output, state, depth + 1, blockPath + ".blocks");
					scope.pop_back();
				}
				continue;
			}

			nlohmann::json resolvedBlock = {{"blockType", blockType}};
			std::string contentFormat = StringValue(block, "contentFormat");
			if(contentFormat.empty())
			{
				contentFormat = "plain-text";
			}
			if(contentFormat != "plain-text" && contentFormat != "html")
			{
				AddDiagnostic(state.result, "pdf_content_format_invalid", "PDF content format must be plain-text or html.", blockPath + ".contentFormat");
			}
			resolvedBlock["contentFormat"] = contentFormat;
			const bool htmlContent = contentFormat == "html";
			if(block.contains("style"))
			{
				nlohmann::json style;
				CopyTextStyle(block.at("style"), style, state.result, blockPath + ".style", true);
				resolvedBlock["style"] = style;
			}

			if(blockType == "paragraph")
			{
				nlohmann::json resolvedRuns;
				ResolveRuns(block.contains("runs") ? block.at("runs") : nlohmann::json::array(), scope, resolvedRuns, state, blockPath + ".runs", htmlContent);
				resolvedBlock["runs"] = resolvedRuns;
			}
			else if(blockType == "list")
			{
				const std::string listKind = StringValue(block, "listKind");
				if(listKind != "bullet" && listKind != "numbered")
				{
					AddDiagnostic(state.result, "pdf_list_kind_invalid", "PDF list must be bullet or numbered.", blockPath + ".listKind");
				}
				resolvedBlock["listKind"] = listKind;
				resolvedBlock["items"] = nlohmann::json::array();
				const nlohmann::json items = block.contains("items") ? block.at("items") : nlohmann::json::array();
				if(!items.is_array())
				{
					AddDiagnostic(state.result, "pdf_list_items_invalid", "PDF list items must be an array.", blockPath + ".items");
				}
				else
				{
					for(size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex)
					{
						const nlohmann::json& item = items[itemIndex];
						nlohmann::json resolvedRuns;
						ResolveRuns(item.is_object() && item.contains("runs") ? item.at("runs") : nlohmann::json::array(), scope, resolvedRuns, state, blockPath + ".items[" + std::to_string(itemIndex) + "].runs", htmlContent);
						resolvedBlock["items"].push_back({{"runs", resolvedRuns}});
					}
				}
			}
			else if(blockType != "page-break")
			{
				AddDiagnostic(state.result, "pdf_block_type_invalid", "Unsupported PDF block type.", blockPath + ".blockType");
			}

			output.push_back(resolvedBlock);
		}

		return state.result.diagnostics.empty();
	}
}

nlohmann::json PdfTemplateResolver::Result::DiagnosticsJson() const
{
	nlohmann::json output = nlohmann::json::array();
	for(const Diagnostic& diagnostic : diagnostics)
	{
		output.push_back({
			{"code", diagnostic.code},
			{"message", diagnostic.message},
			{"path", diagnostic.path}
		});
	}
	return output;
}

PdfTemplateResolver::Result PdfTemplateResolver::Resolve(const RuleSet& ruleSet, const std::string& templateId)
{
	Result result;
	if(templateId.empty())
	{
		AddDiagnostic(result, "pdf_template_id_required", "PDF template ID is required.", "templateId");
		return result;
	}
	if(ruleSet.pdfTemplates == nullptr)
	{
		AddDiagnostic(result, "pdf_templates_unavailable", "The active Session Model contains no PDF templates.", "pdfTemplates");
		return result;
	}

	const nlohmann::json* selectedTemplate = nullptr;
	for(const nlohmann::json& candidate : *ruleSet.pdfTemplates)
	{
		if(StringValue(candidate, "id") == templateId)
		{
			selectedTemplate = &candidate;
			break;
		}
	}
	if(selectedTemplate == nullptr)
	{
		AddDiagnostic(result, "pdf_template_not_found", "PDF template was not found in the active Session Model.", "templateId");
		return result;
	}

	const std::string schemaVersion = StringValue(*selectedTemplate, "schemaVersion");
	const std::string rootRegionId = StringValue(*selectedTemplate, "rootRegionId");
	const std::string rootEntityId = StringValue(*selectedTemplate, "rootEntityId");
	if(schemaVersion != "1.0")
	{
		AddDiagnostic(result, "pdf_template_schema_unsupported", "Unsupported PDF template schema version.", "schemaVersion");
	}
	if(rootRegionId.empty())
	{
		AddDiagnostic(result, "pdf_root_region_required", "PDF template Root region ID is required.", "rootRegionId");
	}

	std::shared_ptr<Entity> rootEntity = FindEntity(ruleSet, rootEntityId);
	if(rootEntity == nullptr || !rootEntity->root.value_or(false) || rootEntity->instances.size() != 1 || rootEntity->instances[0] == nullptr)
	{
		AddDiagnostic(result, "pdf_root_instance_invalid", "PDF template must resolve against the singleton Root instance.", "rootEntityId");
		return result;
	}

	nlohmann::json pageSettings = selectedTemplate->contains("pageSettings") ? selectedTemplate->at("pageSettings") : nlohmann::json::object();
	if(!pageSettings.is_object()
		|| StringValue(pageSettings, "pageSize") != "A4"
		|| StringValue(pageSettings, "orientation") != "portrait")
	{
		AddDiagnostic(result, "pdf_page_settings_invalid", "Version one supports A4 portrait PDF templates only.", "pageSettings");
	}
	const double marginTop = NumberValue(pageSettings, "marginTopMillimetres", 20.0);
	const double marginRight = NumberValue(pageSettings, "marginRightMillimetres", 20.0);
	const double marginBottom = NumberValue(pageSettings, "marginBottomMillimetres", 20.0);
	const double marginLeft = NumberValue(pageSettings, "marginLeftMillimetres", 20.0);
	for(const auto& margin : std::vector<std::pair<std::string, double>>{
		{"marginTopMillimetres", marginTop},
		{"marginRightMillimetres", marginRight},
		{"marginBottomMillimetres", marginBottom},
		{"marginLeftMillimetres", marginLeft}})
	{
		if(margin.second < 0 || margin.second > 100)
		{
			AddDiagnostic(result, "pdf_margin_invalid", "PDF page margins must be between 0 and 100 millimetres.", "pageSettings." + margin.first);
		}
	}

	ResolveState state{ruleSet, result};
	std::vector<ScopeEntry> scope = {{rootRegionId, rootEntityId, rootEntity->instances[0]}};
	nlohmann::json resolvedBlocks = nlohmann::json::array();
	ResolveBlocks(selectedTemplate->contains("blocks") ? selectedTemplate->at("blocks") : nlohmann::json::array(), scope, resolvedBlocks, state, 0, "blocks");

	if(!result.diagnostics.empty())
	{
		return result;
	}

	result.document = {
		{"schemaVersion", "1.0"},
		{"templateId", templateId},
		{"templateName", StringValue(*selectedTemplate, "name")},
		{"pageSettings", {
			{"pageSize", "A4"},
			{"orientation", "portrait"},
			{"marginTopMillimetres", marginTop},
			{"marginRightMillimetres", marginRight},
			{"marginBottomMillimetres", marginBottom},
			{"marginLeftMillimetres", marginLeft}
		}},
		{"blocks", resolvedBlocks}
	};
	result.succeeded = true;
	return result;
}

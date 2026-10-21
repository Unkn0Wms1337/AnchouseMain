#include "FakeLoadout.h"

#include "Changer/Inventory/FakeItems.h"
#include "SDK/Econ/CEconItemSchema.h"

namespace
{
    constexpr std::uint32_t StyleUnset = 0xFF;

    struct Equipped
    {
        CEconItemDefinition* Definition;
        int Style;
    };

    std::vector<Equipped> EquippedDefinitions()
    {
        std::vector<Equipped> Out;
        CEconItemSchema* Schema = CEconItemSchema::Get();
        if (!Schema)
            return Out;
        for (const CSOEconItem& Item : FakeItems::EquippedAll())
        {
            // Class 0 is the global loadout (courier, ward, terrain, ...); those
            // definitions must not feed the hero model/particle replacement lists.
            bool HeroSlot = false;
            for (const auto& State : Item.equipped_state())
                if (State.new_class() != 0)
                {
                    HeroSlot = true;
                    break;
                }
            if (!HeroSlot)
                continue;
            if (CEconItemDefinition* Def = Schema->GetItemDefinition(Item.def_index()))
                Out.push_back({Def, Item.style() == StyleUnset ? 0 : static_cast<int>(Item.style())});
        }
        return Out;
    }
}

std::vector<CEconItemDefinition::AssetModifier> FakeLoadout::EquippedModifiers()
{
    std::vector<CEconItemDefinition::AssetModifier> Out;
    for (const Equipped& Item : EquippedDefinitions())
        for (auto& Modifier : Item.Definition->AssetModifiers(Item.Style))
            Out.push_back(std::move(Modifier));
    return Out;
}

std::vector<std::string> FakeLoadout::EquippedModels()
{
    std::vector<std::string> Out;
    for (const Equipped& Item : EquippedDefinitions())
        for (auto& Model : Item.Definition->Models())
            Out.push_back(std::move(Model));
    return Out;
}

std::string FakeLoadout::Find(const std::vector<CEconItemDefinition::AssetModifier>& Modifiers, const char* Type, const char* Asset)
{
    for (const auto& Modifier : Modifiers)
        if (Modifier.Type == Type && Modifier.Asset == Asset && !Modifier.Modifier.empty())
            return Modifier.Modifier;
    return {};
}

FakeLoadout::Source FakeLoadout::FindSource(const char* Type, const char* Asset)
{
    Source Out;
    for (const Equipped& Item : EquippedDefinitions())
    {
        std::string Modifier = Find(Item.Definition->AssetModifiers(Item.Style), Type, Asset);
        if (Modifier.empty())
            continue;
        Out.Modifier = std::move(Modifier);
        Out.Definition = Item.Definition;
        Out.Style = Item.Style;
        return Out;
    }
    return Out;
}

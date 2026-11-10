#pragma once
#include <cstdint>
#include <cstddef>

struct TrophyDef {
    int id;
    char type; // P, G, S, B
    const char* name_en;
    const char* desc_en;
    const char* name_pt;
    const char* desc_pt;
    const char* icon;
};

inline const TrophyDef* GetTrophyDefs(size_t* count) {
    static const TrophyDef kDefs[] = {
        {0, 'P', "Vortech's Bane", "Collect all the trophies", "O Terror de Vortech", "Colete todos os trofeus", "TROP000.PNG"},
        {1, 'B', "There's no Place Like... This.", "Complete 'Follow the LEGO Brick Road'", "Nao Ha Lugar Como... Este.", "Complete Follow the LEGO Brick Road", "TROP001.PNG"},
        {2, 'B', "Chroma Simpson", "Complete 'Meltdown at Sector 7-G'", "Chroma Simpson", "Complete Meltdown at Sector 7-G", "TROP002.PNG"},
        {3, 'B', "Of Mice and Chen", "Complete 'Elements of Surprise'", "De Ratos e Chen", "Complete Elements of Surprise", "TROP003.PNG"},
        {4, 'B', "In the Nick of Timey-Wimey", "Complete 'A Dalektable Adventure'", "Na Hora Temporal", "Complete A Dalektable Adventure", "TROP004.PNG"},
        {5, 'B', "Simply Walked Into... Metropolis", "Complete 'Painting the Town Black'", "Entrando em... Metropolis", "Complete Painting the Town Black", "TROP005.PNG"},
        {6, 'B', "Interdimensional Showdown", "Complete 'Once Upon A Time Machine in the West'", "Duelo Interdimensional", "Complete Once Upon A Time Machine in the West", "TROP006.PNG"},
        {7, 'B', "GLaD to be out of There!", "Complete 'Aperture Science Enrichment Center'", "Feliz por Sair de La!", "Complete Aperture Science Enrichment Center", "TROP007.PNG"},
        {8, 'B', "Riddle Me That!", "Complete 'Riddle-earth'", "Decifre Esta Charada!", "Complete Riddle-earth", "TROP008.PNG"},
        {9, 'B', "Ghost... busted!", "Complete 'The Phantom Zone'", "Fantasma... Detonado!", "Complete The Phantom Zone", "TROP009.PNG"},
        {10, 'B', "Enter Your Initials: 'AAA'", "Complete 'All Your Bricks Are Belong To Us'", "Insira as Iniciais: AAA", "Complete All Your Bricks Are Belong To Us", "TROP010.PNG"},
        {11, 'B', "Zoinks!", "Complete 'Mystery Mansion Mash-Up'", "Caramba!", "Complete Mystery Mansion Mash-Up", "TROP011.PNG"},
        {12, 'B', "Tri Again Next Time", "Complete 'Prime Time'", "Tente Novamente", "Complete Prime Time", "TROP012.PNG"},
        {13, 'B', "Tri Hard with a Vengeance!", "Complete 'The End is Tri'", "Duro de Triplo Vinganca!", "Complete The End is Tri", "TROP013.PNG"},
        {14, 'B', "A Serious Loophole", "Complete 'The Final Dimension'", "Uma Brecha Seria", "Complete The Final Dimension", "TROP014.PNG"},
        {15, 'B', "Time to Break the Rules!", "Rebuild the LEGO Gateway", "Hora de Quebrar as Regras!", "Reconstrua o Portal LEGO", "TROP015.PNG"},
        {16, 'G', "Broke the Rules!", "Complete all story levels", "Quebrou as Regras!", "Complete todas as fases da historia", "TROP016.PNG"},
        {17, 'B', "The Keystone to Victory", "Collect all of the Gateway Keystones", "A Chave para a Vitoria", "Colete todas as Pedras Fundamentais do Portal", "TROP017.PNG"},
        {18, 'G', "Rare Artefacts, collected!", "Collect 30 Minikits from levels", "Artefatos Raros Coletados!", "Colete 30 Minikits das fases", "TROP018.PNG"},
        {19, 'S', "Na na na na na na na upgrades!", "Upgrade the Batmobile to Model 2", "Melhorias do Batmovel 2", "Melhore o Batmovel para o Modelo 2", "TROP019.PNG"},
        {20, 'G', "Holy Upgrades, Batman!", "Upgrade the Batmobile to Model 3", "Santas Melhorias, Batman!", "Melhore o Batmovel para o Modelo 3", "TROP020.PNG"},
        {21, 'B', "Home, Sweet Home", "Visit any Hub World for the First Time", "Lar, Doce Lar", "Visite qualquer Mundo Hub pela primeira vez", "TROP021.PNG"},
        {22, 'B', "Dimensional Explorer", "Visit the homes of Batman, Gandalf and Wyldstyle", "Explorador Dimensional", "Visite as casas de Batman, Gandalf e Megaestilo", "TROP022.PNG"},
        {23, 'B', "Time for an Upgrade!", "Collect 5 Gold Bricks", "Hora da Melhoria!", "Colete 5 Tijolos de Ouro", "TROP023.PNG"},
        {24, 'S', "The Real Golden Master!", "Collect 30 Gold Bricks", "Mestre Dourado Verdadeiro!", "Colete 30 Tijolos de Ouro", "TROP024.PNG"},
        {25, 'B', "Dimensional Dasher", "Complete a race in any of the hub worlds", "Corredor Dimensional", "Complete uma corrida em qualquer mundo hub", "TROP025.PNG"},
        {26, 'S', "Pedal to the Metal", "Complete 5 hub races", "Pe na Tabua", "Complete 5 corridas em mundos hub", "TROP026.PNG"},
        {27, 'B', "The Heroes We Need", "Complete any hub world quest", "Os Herois Necessarios", "Complete qualquer missao de mundo hub", "TROP027.PNG"},
        {28, 'B', "Restoring Order", "Complete any hub world renovation", "Restaurando a Ordem", "Complete qualquer reforma de mundo hub", "TROP028.PNG"},
        {29, 'S', "Official Master Builder", "Complete 15 hub world renovations", "Mestre Construtor Oficial", "Complete 15 reformas de mundos hub", "TROP029.PNG"},
        {30, 'S', "Taking the Tube", "Travel through 20 rifts throughout the game", "Pegando o Tubo", "Viaje por 20 fendas dimensionais no jogo", "TROP030.PNG"},
        {31, 'S', "Speedy Thing Goes In...", "Complete 'Aperture Science Enrichment Center' within 25 minutes", "Coisa Veloz Entra...", "Complete Aperture Science em ate 25 minutos", "TROP031.PNG"},
        {32, 'S', "Forever Alone", "Complete a level while only ever keeping one character active on the Toy Pad", "Sempre Sozinho", "Complete uma fase com apenas um personagem no Toy Pad", "TROP032.PNG"},
        {33, 'S', "Lost Property", "Complete 25 'Locate Mode' puzzles", "Achados e Perdidos", "Complete 25 quebra-cabecas no Modo Localizar", "TROP033.PNG"},
        {34, 'S', "Painting by Numbers", "Complete 25 'Chroma Mode' puzzles", "Pintando por Numeros", "Complete 25 quebra-cabecas no Modo Croma", "TROP034.PNG"},
        {35, 'S', "Elemental, My Dear", "Enter 'Elemental Mode' on 25 unique occasions", "Elementar, Meu Caro", "Ative o Modo Elemental em 25 ocasioes", "TROP035.PNG"},
        {36, 'S', "Too Old for This Shift", "Shift through 50 'Shift Mode' rifts", "Velho Demais Para Mudar", "Mude atraves de 50 fendas no Modo Mudar", "TROP036.PNG"},
        {37, 'S', "But Does it Come in Pink?", "Give the Batmobile a pink paint job", "Mas Vem em Cor-de-Rosa?", "Pinte o Batmovel de cor-de-rosa", "TROP037.PNG"},
        {38, 'G', "Do You Like Bling, Batman?", "Give the Batmobile a chrome paint job", "Gosta de Brilho, Batman?", "Pinte o Batmovel de cromado", "TROP038.PNG"},
        {39, 'B', "In your face, space coyote!", "Complete 'The Mysterious Voyage of Homer'", "Coiote Espacial!", "Complete The Mysterious Voyage of Homer", "TROP039.PNG"},
        {40, 'B', "Mmmm... collectables!", "Collect any Minikit within the 'The Mysterious Voyage of Homer' level", "Mmmm... Colecionaveis!", "Colete qualquer Minikit em The Mysterious Voyage of Homer", "TROP040.PNG"},
        {41, 'B', "Owww, rules? I hate rules!", "Achieve the 'Rule Breaker' stud goal in the 'The Mysterious Voyage of Homer' level", "Regras? Eu Odeio Regras!", "Atinja a meta Quebrador de Regras com Homer", "TROP041.PNG"},
        {42, 'B', "That was heavy!", "Complete 'Back to the Future!'", "Isso Foi Pesado!", "Complete Back to the Future!", "TROP042.PNG"},
        {43, 'B', "Mr. Fusion Home Energy Reactor", "Collect any Minikit within the 'Back to the Future!' level", "Reator de Fusao Caseiro", "Colete qualquer Minikit em Back to the Future!", "TROP043.PNG"},
        {44, 'B', "Rules? ...We don't need rules!", "Achieve the 'Rule Breaker' stud goal in the 'Back to the Future!' level", "Nao Precisamos de Regras!", "Atinja a meta Quebrador de Regras em Back to the Future!", "TROP044.PNG"},
        {45, 'B', "It's been fun. Don't come back", "Complete 'Aperture Science'", "Foi Divertido. Nao Volte", "Complete Aperture Science", "TROP045.PNG"},
        {46, 'B', "When life gives you Minikits...", "Collect any Minikit within the 'Aperture Science' level", "Quando a Vida Te Da Minikits", "Colete qualquer Minikit em Aperture Science", "TROP046.PNG"},
        {47, 'B', "For science... you monster", "Achieve the 'Rule Breaker' stud goal in the 'Aperture Science' level", "Pela Ciencia... Monstro", "Atinja a meta Quebrador de Regras em Aperture Science", "TROP047.PNG"},
        {48, 'B', "Our destiny is in the stars", "Complete 'The Dalek Extermination of Earth'", "Destino Esta nas Estrelas", "Complete The Dalek Extermination of Earth", "TROP048.PNG"},
        {49, 'B', "A Minikit! Minikits are cool!", "Collect any Minikit within the 'The Dalek Extermination of Earth' level", "Minikits Sao Incriveis!", "Colete qualquer Minikit em Dalek Extermination of Earth", "TROP049.PNG"},
        {50, 'B', "People come along and break them", "Achieve the 'Rule Breaker' stud goal in the 'The Dalek Extermination of Earth' level", "Feitas Para Serem Quebradas", "Atinja a meta Quebrador de Regras com os Daleks", "TROP050.PNG"},
        {51, 'B', "I ain't afraid of no Ghosts!", "Complete 'Ghostbusters!'", "Nao Tenho Medo de Fantasmas!", "Complete Ghostbusters!", "TROP051.PNG"},
        {52, 'B', "WE GOT ONE!!!", "Collect any Minikit within the 'Ghostbusters!' level", "PEGAMOS UM!!!", "Colete qualquer Minikit em Ghostbusters!", "TROP052.PNG"},
        {53, 'B', "Rule breakin' makes me feel good", "Achieve the 'Rule Breaker' stud goal in the 'Ghostbusters!' level", "Quebrar Regras Faz Bem", "Atinja a meta Quebrador de Regras em Ghostbusters!", "TROP053.PNG"},
        {54, 'B', "New High Score", "Complete 'Retro Wreckage'", "Novo Recorde", "Complete Retro Wreckage", "TROP054.PNG"},
        {55, 'B', "Power Up!", "Obtain any Minikit within 'Retro Wreckage'", "Power Up!", "Obtenha qualquer Minikit em Retro Wreckage", "TROP055.PNG"},
        {56, 'B', "Insert Stud", "Achieve the 'Rule Breaker' stud goal in 'Retro Wreckage'", "Insira Pecas", "Atinja a meta Quebrador de Regras em Retro Wreckage", "TROP056.PNG"},
        {57, 'B', "The fun will never end!", "Complete 'A Book and a Bad Guy'", "A Diversao Nunca Termina!", "Complete A Book and a Bad Guy", "TROP057.PNG"},
        {58, 'B', "Another for the Treasure Room!", "Collect any Minikit in the 'A Book and a Bad Guy' level", "Mais Um Para a Sala do Tesouro!", "Colete qualquer Minikit em A Book and a Bad Guy", "TROP058.PNG"},
        {59, 'B', "Mathematical!", "Achieve the 'Rule Breaker' stud goal in the 'A Book and a Bad Guy level", "Matematico!", "Atinja a meta Quebrador de Regras em A Book and a Bad Guy", "TROP059.PNG"},
        {60, 'B', "Avoided The Cataclysm", "Complete all of the levels in the Ghostbusters (2016) Story Pack", "Cataclismo Evitado", "Complete todas as fases de Ghostbusters 2016", "TROP060.PNG"},
        {61, 'B', "Quest Busters", "Complete any quest in the Ghostbusters (2016) Adventure World", "Cacadores de Missoes", "Complete qualquer missao em Ghostbusters 2016", "TROP061.PNG"},
        {62, 'B', "Rule-Breaking the Barrier", "Achieve the 'Rule Breaker' stud goal in any Ghostbusters (2016) Story Pack level", "Rompendo a Barreira", "Atinja a meta Quebrador de Regras em Ghostbusters 2016", "TROP062.PNG"},
        {63, 'B', "Mission: Accomplished", "Complete 'Mission: Impossible'", "Missao Cumprida", "Complete Mission: Impossible", "TROP063.PNG"},
        {64, 'B', "Wouldn't have it any other way", "Collect any Minikit in the 'Mission: Impossible' level", "Nao Faria de Outro Jeito", "Colete qualquer Minikit em Mission: Impossible", "TROP064.PNG"},
        {65, 'B', "Good Morning, Mr Hunt...", "Achieve the 'Rule Breaker' stud goal in the 'Mission: Impossible' level", "Bom Dia, Sr. Hunt...", "Atinja a meta Quebrador de Regras em Mission: Impossible", "TROP065.PNG"},
        {66, 'B', "No-maj, No-problem", "Complete all of the levels in the Fantastic Beasts Story Pack", "Sem Magia, Sem Problema", "Complete todas as fases de Animais Fantasticos", "TROP066.PNG"},
        {67, 'B', "Manhattan Tourist", "Complete any quest in the Fantastic Beasts Adventure World", "Turista em Manhattan", "Complete qualquer missao em Animais Fantasticos", "TROP067.PNG"},
        {68, 'B', "More shinies than a niffler", "Achieve the 'Rule Breaker' stud goal in any Fantastic Beasts Story Pack level", "Mais Brilho que um Pelucio", "Atinja a meta Quebrador de Regras em Animais Fantasticos", "TROP068.PNG"},
        {69, 'B', "Chaos Theory", "Complete 'Sonic Dimensions'", "Teoria do Caos", "Complete Sonic Dimensions", "TROP069.PNG"},
        {70, 'B', "I usually collect Emeralds...", "Collect any Minikit in the 'Sonic Dimensions' level", "Eu Costumo Coletar Esmeraldas", "Colete qualquer Minikit em Sonic Dimensions", "TROP070.PNG"},
        {71, 'B', "Ring Breaker", "Achieve the 'Rule Breaker' stud goal in the 'Sonic Dimensions' level", "Quebrador de Aneis", "Atinja a meta Quebrador de Regras em Sonic Dimensions", "TROP071.PNG"},
        {72, 'B', "Make a change", "Complete all the levels in the The LEGO Batman Movie Story Pack.", "Faca a Mudanca", "Complete todas as fases de LEGO Batman O Filme", "TROP072.PNG"},
        {73, 'B', "I've been 'Batmanning'", "Complete any quest in the The LEGO Batman Movie Adventure World.", "Sendo o Batman", "Complete qualquer missao em LEGO Batman O Filme", "TROP073.PNG"},
        {74, 'B', "Bat-Dollars", "Achieve the 'Rule Breaker' stud goal in any The LEGO Batman Movie Story Pack level", "Bat-Dolares", "Atinja a meta Quebrador de Regras em LEGO Batman O Filme", "TROP074.PNG"},
        {75, 'B', "Declared a fellow Goonie", "Complete 'The Goonies'", "Declarado um Goonie", "Complete The Goonies", "TROP075.PNG"},
        {76, 'B', "A pirate's bounty", "Collect any Minikit in 'The Goonies' level", "Tesouro de Pirata", "Colete qualquer Minikit em The Goonies", "TROP076.PNG"},
        {77, 'B', "Hey, you guys!", "Achieve the 'Rule Breaker' stud goal in the 'The Goonies' level", "Ei, Voces Ai!", "Atinja a meta Quebrador de Regras em The Goonies", "TROP077.PNG"},
    };
    if (count) *count = sizeof(kDefs) / sizeof(kDefs[0]);
    return kDefs;
}

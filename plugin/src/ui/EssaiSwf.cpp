#include "ui/EssaiSwf.h"

#include "ui/Etabli.h"

// Voir EssaiSwf.h pour ce qui est éprouvé. Ici : un RE::IMenu minimal qui
// demande au moteur de charger NOTRE film, et raconte tout au journal.

namespace FUI::EssaiSwf
{
    namespace
    {
        /* SANS EXTENSION, et c'est vital. BSScaleformManager::BuildFilePath
           la pose LUI-MÊME :

               filePath = "Interface/"; filePath += a_fileName; filePath += ".swf";

           puis retombe sur "Interface/Exported/<nom>.gfx". Écrire
           « vulkaar_essai.swf » ici lui aurait fait chercher
           « Interface/vulkaar_essai.swf.swf » — introuvable — et LoadMovieEx
           serait sorti en FAUX AVANT d'appeler CreateMovie, donc SANS jamais
           invoquer notre rappel. On aurait lu ce faux comme un refus de
           Scaleform, et abandonné toute une voie sur un nom de fichier. */
        /* LE SOUS-DOSSIER PASSE — eprouve le 29/08 : « vulkaar/etabli » a rendu
           TROUVE, avec la barre oblique, exactement comme BuildFilePath ecrit
           « Interface/ ». Le FAUX qu'on avait lu quelques minutes plus tot ne
           venait PAS du chemin : le joueur avait lance pendant que le launcher
           posait encore le fichier. La lecon est de methode — un FAUX SANS la
           ligne « film analyse » veut dire INTROUVABLE, et la premiere chose a
           verifier est que le fichier est bien arrive, pas que le chemin est
           mal forme. Ne jamais mettre l'extension : BuildFilePath la pose. */
        constexpr const char* kChemin = "vulkaar/etabli";
        constexpr std::string_view kNom = "vulkaarEssaiSwf";

        bool g_ouvert = false;

        /* ── LE PONT, SENS ACTIONSCRIPT -> C++ ────────────────────────────
           On derive GFxFunctionHandler (RE/G/GFxFunctionHandler.h) : une
           classe PURE VIRTUELLE de CommonLibSSE, rien de Bethesda. Le moteur
           en fait une valeur ActionScript par CreateFunction, on la pose dans
           _root, et le film l'appelle comme une fonction ordinaire.

           C'est la voie choisie contre celle de SkyUI : gfx.io.GameDelegate
           est compile A L'INTERIEUR des swf du jeu — mesure, extrait de
           craftingmenu.swf — donc inemployable sans redistribuer du Bethesda.

           Le handler est compte par reference (GRefCountBase) : il se tient
           par un GPtr, et ce GPtr doit SURVIVRE AU FILM. D'ou le static. */
        class Recepteur : public RE::GFxFunctionHandler
        {
        public:
            void Call(Params& a_params) override
            {
                const auto arg = [&](std::uint32_t i) -> const char* {
                    if (a_params.args == nullptr || i >= a_params.argCount) return "";
                    const auto& v = a_params.args[i];
                    return v.IsString() ? v.GetString() : "(pas une chaine)";
                };
                SKSE::log::info("[PONT] AS -> C++ : {} argument(s) — « {} » « {} »",
                    a_params.argCount, arg(0), arg(1));

                /* L'ETABLI FAIT FOI. Le film ne tient aucun etat : il demande,
                   le C++ decide, et le plateau redescend. C'est la meme regle
                   que pour le serveur vis-a-vis du C++ — chaque etage ne fait
                   que MONTRER et DEMANDER a celui du dessous. */
                const bool repeindre = FUI::Etabli::ActionDuFilm(arg(0), arg(1));
                if (repeindre && a_params.movie != nullptr) {
                    const std::string plateau = FUI::Etabli::PlateauPourFilm();
                    /* La chaine doit VIVRE jusqu'apres l'appel : GFxValue ne
                       copie rien, il garde le pointeur. */
                    a_params.movie->SetVariable("_root.vkPlateau", plateau.c_str());
                    /* Invoke, pas InvokeNoReturn : celui-la appartient a
                       GFxMovieView, et le rappel ne nous donne qu'un
                       GFxMovie — sa base. */
                    a_params.movie->Invoke("_root.vkPeindre", nullptr, nullptr, 0);
                }
                if (a_params.retVal != nullptr) {
                    a_params.retVal->SetBoolean(true);
                }
            }
        };
        RE::GPtr<Recepteur> g_recepteur;

        class MenuEssai : public RE::IMenu
        {
        public:
            MenuEssai()
            {
                using Flags = RE::UI_MENU_FLAGS;
                /* PAS de kCustomRendering : on veut justement que le moteur
                   dessine le film LUI-MÊME. C'est toute la différence avec nos
                   écrans ImGui, qui peignent à la main dans PostDisplay. */
                menuFlags.set(Flags::kUsesCursor, Flags::kUpdateUsesCursor);
                menuFlags.set(Flags::kAllowSaving);
                depthPriority = 12;
                inputContext = Context::kMenuMode;

                auto* sm = RE::BSScaleformManager::GetSingleton();
                if (sm == nullptr) {
                    SKSE::log::error("[ESSAI-SWF] BSScaleformManager introuvable — rien a dire");
                    return;
                }

                const bool charge = sm->LoadMovieEx(this, kChemin,
                        RE::GFxMovieView::ScaleModeType::kShowAll, [](RE::GFxMovieDef* def) {
                            if (def == nullptr) {
                                SKSE::log::warn("[ESSAI-SWF] rappel avec un GFxMovieDef NUL");
                                return;
                            }
                            SKSE::log::info(
                                "[ESSAI-SWF] film analyse : {} x {} px, version {}, {} image(s), {} i/s",
                                def->GetWidth(), def->GetHeight(), def->GetVersion(),
                                def->GetFrameCount(), def->GetFrameRate());
                        });
                SKSE::log::info("[ESSAI-SWF] chemin retenu : {} -> {}",
                    kChemin,
                    charge ? "VRAI — Scaleform a pris notre swf" : "FAUX — fichier INTROUVABLE (le rappel n'a pas eu lieu)");
                SKSE::log::info("[ESSAI-SWF] uiMovie apres chargement : {}",
                    uiMovie ? "present" : "NUL");
                if (uiMovie) {
                    /* LE TEMOIN DE L'ACTIONSCRIPT. Le film pose _global.vulkaarPret
                       a vrai a la fin de sa premiere image. Le lire prouve que
                       Scaleform n'a pas seulement charge le film : il a EXECUTE
                       notre code. C'est la marche d'apres « uiMovie present ». */
                    RE::GFxValue pret;
                    const bool lu = uiMovie->GetVariable(&pret, "_global.vulkaarPret");
                    SKSE::log::info("[ESSAI-SWF] _global.vulkaarPret : {}",
                        !lu ? "ILLISIBLE — l'ActionScript n'a pas tourne"
                            : (pret.GetBool() ? "VRAI — notre ActionScript s'est execute"
                                              : "faux — pose mais pas atteint"));
                    BrancherLePont();
                }
            }

            /* Les trois marches du pont, dans l'ordre, chacune journalisee :
               poser la fonction de retour, pousser une chaine, faire repeindre.
               Une marche qui echoue se voit tout de suite, et on sait laquelle. */
            void BrancherLePont()
            {
                if (!g_recepteur) {
                    g_recepteur = RE::make_gptr<Recepteur>();
                }

                // ── 1. AS -> C++ : _root.vkRecevoir devient notre fonction ──
                RE::GFxValue fn;
                uiMovie->CreateFunction(&fn, g_recepteur.get());
                const bool posee = uiMovie->SetVariable("_root.vkRecevoir", fn);
                SKSE::log::info("[PONT] CreateFunction + SetVariable(_root.vkRecevoir) -> {}",
                    posee ? "posee" : "REFUSEE");

                // Relire prouve que la valeur a bien pris de l'autre cote.
                RE::GFxValue relue;
                if (uiMovie->GetVariable(&relue, "_root.vkRecevoir")) {
                    SKSE::log::info("[PONT] relecture de _root.vkRecevoir : type {}",
                        static_cast<int>(relue.GetType()));
                } else {
                    SKSE::log::warn("[PONT] _root.vkRecevoir ILLISIBLE apres pose");
                }

                /* ── 2. C++ -> AS : LE VRAI PLATEAU ──────────────────────────
                   Ce que le serveur a pousse pour CETTE station et CE joueur,
                   filtre par le rayon et la recherche en cours. Les
                   ingredients du seul geste choisi partent avec — envoyer
                   ceux des 5 586 gestes de la forge ferait 1,9 Mio.

                   LA CHAINE DOIT VIVRE JUSQU APRES L APPEL : GFxValue ne
                   copie rien, il garde le pointeur. Une temporaire ici
                   enverrait a Scaleform de la memoire liberee. */
                const std::string plateau = FUI::Etabli::PlateauPourFilm();
                const bool pousse = uiMovie->SetVariable("_root.vkPlateau", plateau.c_str());
                SKSE::log::info("[PONT] SetVariable(_root.vkPlateau, {} octets) -> {}",
                    plateau.size(), pousse ? "pousse" : "REFUSE");

                // ── 3. C++ -> AS : on demande la repeinture ──
                uiMovie->InvokeNoReturn("_root.vkPeindre", nullptr, 0);
                SKSE::log::info("[PONT] Invoke(_root.vkPeindre) demande");
            }

            static RE::IMenu* Creer() { return new MenuEssai(); }

            RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override
            {
                return RE::IMenu::ProcessMessage(a_message);
            }
        };
    }

    void Initialiser()
    {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->Register(kNom, MenuEssai::Creer);
            SKSE::log::info("[ESSAI-SWF] menu « {} » inscrit — F9 pour l'ouvrir", kNom);
        } else {
            SKSE::log::error("[ESSAI-SWF] RE::UI introuvable : rien n'est inscrit");
        }
    }

    void Basculer()
    {
        auto* q = RE::UIMessageQueue::GetSingleton();
        if (q == nullptr) {
            SKSE::log::error("[ESSAI-SWF] UIMessageQueue introuvable");
            return;
        }
        const RE::BSFixedString nom{ kNom };
        if (g_ouvert) {
            q->AddMessage(nom, RE::UI_MESSAGE_TYPE::kHide, nullptr);
            g_ouvert = false;
            SKSE::log::info("[ESSAI-SWF] fermeture demandee");
            return;
        }
        SKSE::log::info("[ESSAI-SWF] ---- ouverture demandee (F9) ----");
        q->AddMessage(nom, RE::UI_MESSAGE_TYPE::kShow, nullptr);
        g_ouvert = true;
    }
}
